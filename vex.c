/* vex.c : vanilla-extract main */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <google/protobuf-c/protobuf-c.h> // contains varint functions
#include "intpack.h"
#include "pbf.h"
#include "tags.h"

// 14 bits -> 1.7km at 45 degrees
// 13 bits -> 3.4km at 45 degrees
// at 45 degrees cos(pi/4)~=0.7
// TODO maybe shift one more bit off of y to make bins more square
#define GRID_BITS 14
/* The width and height of the grid root is 2^bits. */
#define GRID_DIM (1 << GRID_BITS)

/*
  https://taginfo.openstreetmap.org/reports/database_statistics
  There are over 10 times as many nodes as ways in OSM.
  Assume there are as many active node references as there are active and deleted nodes.
*/
#define MAX_NODE_ID   4000000000
#define MAX_WAY_ID     400000000
#define MAX_NODE_REFS 4000000000

/* Way reference block size is based on the typical number of ways per grid cell. */
#define WAY_BLOCK_SIZE 32
/* Assume one-fifth as many blocks as cells in the grid. Observed number is ~15000000 blocks. */
#define MAX_WAY_BLOCKS (GRID_DIM * GRID_DIM / 5)

/* If true, then loaded file should not be persisted to disk. */
static bool in_memory;

/* 
  Define the sequence in which elements are read and written, while allowing element types as 
  function parameters and array indexes.
*/
#define NODE 0
#define WAY  1
#define RELATION 2

/* The location where we will save all files. This can be set using a command line parameter. */
static const char *database_path;

/* Compact geographic position. Latitude and longitude mapped to the signed 32-bit int range. */
typedef struct {
    int32_t x;
    int32_t y;
} coord_t;

/* Convert double-precision floating point latitude and longitude to internal representation. */
static void to_coord (/*OUT*/ coord_t *coord, double lat, double lon) {
    coord->x = (lon * INT32_MAX) / 180;
    coord->y = (lat * INT32_MAX) / 90;
} // TODO this is a candidate for return by value

/* Converts the y field of a coord to a floating point latitude. */
static double get_lat (coord_t *coord) {
    return ((double) coord->y) * 90 / INT32_MAX;
}

/* Converts the x field of a coord to a floating point longitude. */
static double get_lon (coord_t *coord) {
    return ((double) coord->x) * 180 / INT32_MAX;
}

/* A block of way references. Chained together to record which ways begin in each grid cell. */
typedef struct {
    int32_t refs[WAY_BLOCK_SIZE];
    uint32_t next; // index of next way block, or number of free slots if negative
} WayBlock;

/*
  A single OSM node. An array of 2^64 these serves as a map from node ids to nodes.
  OSM assigns node IDs sequentially, so you only need about the first 2^32 entries as of 2014.
  Note that when nodes are deleted their IDs are not reused, so there are holes in
  this range, but sparse file support in the filesystem should take care of that.
  "Deleted node ids must not be reused, unless a former node is now undeleted."
*/
typedef struct {
    coord_t coord; // compact internal representation of latitude and longitude
    uint32_t tags; // byte offset into the packed tags array where this node's tag list begins
} Node;

/*
  A single OSM way. Like nodes, way IDs are assigned sequentially, so a zero-indexed array of these
  serves as a map from way IDs to ways.
*/
typedef struct {
    uint32_t node_ref_offset; // the index of the first node in this way's node list
    uint32_t tags; // byte offset into the packed tags array where this node's tag list begins
} Way;

/*
  The spatial index grid. A node's grid bin is determined by right-shifting its coordinates.
  Initially this was a multi-level grid, but it turns out to work fine as a single level.
  Rather than being directly composed of way reference blocks, there is a level of indirection
  because the grid is mostly empty due to ocean and wilderness. TODO eliminate coastlines etc.
*/
typedef struct {
    uint32_t cells[GRID_DIM][GRID_DIM]; // contains indexes to way_blocks
} Grid;

/* File descriptor for the lockfile. */
/* Use BSD-style locks which are associated with the file, not the process. */
static int lock_fd; 

/* Print human readable representation based on multiples of 1024 into a static buffer. */
static char human_buffer[128];
char *human (size_t bytes) {
    /* Convert to a double, so division can yield results with decimal places. */
    double s = bytes;
    if (s < 1024) {
        sprintf (human_buffer, "%.1lf ", s);
        return human_buffer;
    }
    s /= 1024;
    if (s < 1024) {
        sprintf (human_buffer, "%.1lf ki", s);
        return human_buffer;
    }
    s /= 1024;
    if (s < 1024) {
        sprintf (human_buffer, "%.1lf Mi", s);
        return human_buffer;
    }
    s /= 1024;
    if (s < 1024) {
        sprintf (human_buffer, "%.1lf Gi", s);
        return human_buffer;
    }
    s /= 1024;
    sprintf (human_buffer, "%.1lf Ti", s);
    return human_buffer;
}

void die (char *s) {
    printf("%s\n", s);
    exit(EXIT_FAILURE);
}

/* Make a filename under the database directory, performing some checks. */
static char path_buf[512];
static char *make_db_path (const char *name, uint32_t subfile) {
    if (strlen(name) >= sizeof(path_buf) - strlen(database_path) - 12)
        die ("Name too long.");
    if (in_memory) {
        sprintf (path_buf, "vex_%s.%d", name, subfile);
    } else {
        size_t path_length = strlen(database_path);
        if (path_length == 0)
            die ("Database path must be non-empty.");
        if (database_path[path_length - 1] == '/')
            path_length -= 1;
        if (subfile == 0)
            sprintf (path_buf, "%.*s/%s", (int)path_length, database_path, name);
        else
            sprintf (path_buf, "%.*s/%s.%03d", (int)path_length, database_path, name, subfile);
    }
    return path_buf;
}

/*
  Map a file in the database directory into memory, letting the OS handle paging.
  Note that we cannot reliably re-map a file to the same memory address, so the files should not
  contain pointers. Instead we store array indexes, which can have the advantage of being 32-bits
  wide. We map one file per OSM object type.

  Mmap will happily map a zero-length file to a nonzero-length block of memory, but a bus error
  will occur when you try to write to the memory.

  It is tricky to expand the mapped region on demand you'd need to trap the bus error.
  Instead we reserve enough address space for the maximum size we ever expect the file to reach.
  Linux provides the mremap() system call for expanding or shrinking the size of a given mapping.
  msync() flushes the changes in memory to disk.

  The ext3 and ext4 filesystems understand "holes" via the sparse files mechanism:
  http://en.wikipedia.org/wiki/Sparse_file#Sparse_files_in_Unix
  Creating 100GB of empty file by calling truncate() does not increase the disk usage.
  The files appear to have their full size using 'ls', but 'du' reveals that no blocks are in use.
*/
void *map_file(const char *name, uint32_t subfile, size_t size) {
    make_db_path (name, subfile);
    int fd;
    if (in_memory) {
        printf("Opening shared memory object '%s' of size %sB.\n", path_buf, human(size));
        fd = shm_open(path_buf, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    } else {
        printf("Mapping file '%s' of size %sB.\n", path_buf, human(size));
        // including O_TRUNC causes much slower write (swaps pages in?)
        fd = open(path_buf, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    }
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
        die("Could not memory map file.");
    if (ftruncate (fd, size - 1)) // resize file
        die ("Error resizing file.");
    return base;
}

/* Open a buffered append FILE in the current working directory, performing some checks. */
FILE *open_output_file(const char *name, uint8_t subfile) {
    printf("Opening file '%s' as append stream.\n", name);
    FILE *file = fopen(name, "a"); // Creates if file does not exist.
    if (file == NULL) die("Could not open file for output.");
    return file;
}

/* Arrays of memory-mapped structs. This is where we store the bulk of our data. */
Grid     *grid;
Node     *nodes;
Way      *ways;
WayBlock *way_blocks;
int64_t  *node_refs;  // A negative node_ref marks the end of a list of refs.
uint32_t n_node_refs; // The number of node refs currently used.
// FIXME n_node_refs will eventually overflow. The fact that it's unsigned gives us a little slack.

/*
  The number of way reference blocks currently allocated.
  Sparse files appear to be full of zeros until you write to them. Therefore we skip way block zero
  so we can use the zero index to mean "no way block".
*/
uint32_t way_block_count = 1;
static uint32_t new_way_block() {
    if (way_block_count % 100000 == 0)
        printf("%dk way blocks in use out of %dk.\n", way_block_count/1000, MAX_WAY_BLOCKS/1000);
    if (way_block_count >= MAX_WAY_BLOCKS)
        die("More way reference blocks are used than expected.");
    // A negative value in the last ref entry gives the number of free slots in this block.
    way_blocks[way_block_count].refs[WAY_BLOCK_SIZE-1] = -WAY_BLOCK_SIZE;
    // printf("created way block %d\n", way_block_count);
    return way_block_count++;
}

/* Get the x or y bin for the given x or y coordinate. */
static uint32_t bin (int32_t xy) {
    return ((uint32_t)(xy)) >> (32 - GRID_BITS); // unsigned: logical shift
}

/* Get the address of the grid cell for the given internal coord. */
static uint32_t *get_grid_cell(coord_t coord) {
    return &(grid->cells[bin(coord.x)][bin(coord.y)]);
}

/*
  Given a Node struct, return the index of the way reference block at the head of the Node's grid
  cell, creating a new way reference block if the grid cell is currently empty.
*/
static uint32_t get_grid_way_block (Node *node) {
    uint32_t *cell = get_grid_cell(node->coord);
    if (*cell == 0) {
        *cell = new_way_block();
    }
    // printf("xbin=%d ybin=%d\n", xb, yb);
    // printf("index=%d\n", index);
    return *cell;
}

/* TODO make this insert a new block instead of just setting the grid cell contents. */
static void set_grid_way_block (Node *node, uint32_t wbi) {
    uint32_t *cell = get_grid_cell(node->coord);
    *cell = wbi;
}

/* A memory block holding tags for a sub-range of the OSM ID space. */
typedef struct {
    uint8_t *data;
    size_t pos;
} TagSubfile;

#define MAX_SUBFILES 32
static TagSubfile tag_subfiles[MAX_SUBFILES] = {[0 ... MAX_SUBFILES - 1] {.data=NULL, .pos=0}};

/*
  The ID space must be split up.
  Most tags are on ways. There are about 10 times as many nodes as ways, and 100 times less
  relations than so we divide way IDs and multiply relation IDs to spread them evenly across
  the range of way IDs.
*/
static uint32_t subfile_index_for_id (int64_t osmid, int entity_type) {
    if (entity_type == NODE) osmid /= 16;
    else if (entity_type == RELATION) osmid *= 64;
    uint32_t subfile = osmid >> 25; // split the way id space into sub-ranges of 33 million IDs.
    return subfile;
}

/* Get the subfile in which the tags for the given OSM entity should be stored. */
static TagSubfile *tag_subfile_for_id (int64_t osmid, int entity_type) {
    uint32_t subfile = subfile_index_for_id (osmid, entity_type);
    if (subfile >= MAX_SUBFILES) die ("Need more subfiles than expected.");
    TagSubfile *ts = &(tag_subfiles[subfile]);
    if (ts->data == NULL) {
        // Lazy-map a subfile when needed.
        ts->data = map_file("tags", subfile, UINT32_MAX); // all files are 4GB sparse maps
        ts->pos = 0;
    }
    return ts;
}

/* 
  Grab a pointer to tag subfile data directly. Convenience method to avoid manually dereferencing. 
  This does not seek to the element within the tag file, it returns the beginning adress.
  TODO perform the seek here as well?
*/
static uint8_t *tag_data_for_id (int64_t osmid, int entity_type) {
    return tag_subfile_for_id(osmid, entity_type)->data;
}

/* Write a ProtobufCBinaryData out to a TagSubfile, updating the subfile position accordingly. */
static void ts_write(ProtobufCBinaryData *bd, TagSubfile *ts) {
    uint8_t *dst = ts->data + ts->pos;
    uint8_t *src = bd->data;
    for (int i = 0; i < bd->len; i++) *(dst++) = *(src++);
    ts->pos += bd->len;
}

/* Write a ProtobufCBinaryData out to a TagSubfile, updating the subfile position accordingly. */
static void ts_putc(char c, TagSubfile *ts) {
    ts->data[(ts->pos)++] = c;
}

/*
  Given parallel tag key and value arrays of length n containing string table indexes,
  write compacted lists of key=value pairs to a file which do not require the string table.
  Returns the byte offset of the beginning of the new tag list within that file.
*/
static uint32_t write_tags (uint32_t *keys, uint32_t *vals, int n, ProtobufCBinaryData *string_table, TagSubfile *ts) {
    /* If there are no tags, point to index 0, which contains a single tag list terminator char. */
    if (n == 0) return 0;
    uint64_t position = ts->pos;
    if (position > UINT32_MAX) die ("A tag file index has overflowed.");
    int n_tags_written = 0;
    for (int t = 0; t < n; t++) {
        ProtobufCBinaryData key = string_table[keys[t]];
        ProtobufCBinaryData val = string_table[vals[t]];
        // skip unneeded keys
        if (memcmp("created_by",  key.data, key.len) == 0 ||
            memcmp("import_uuid", key.data, key.len) == 0 ||
            memcmp("attribution", key.data, key.len) == 0 ||
            memcmp("source",      key.data, 6) == 0 ||
            memcmp("tiger:",      key.data, 6) == 0) {
            continue;
        }
        int8_t code = encode_tag(key, val);
        // Code always written out to encode a key and/or a value, or indicate they are free text.
        ts_putc(code, ts);
        if (code == 0) {
            // Code 0 means zero-terminated key and value are written out in full.
            // Saving only tags with 'known' keys (nonzero codes) cuts file sizes in half.
            // Some are reduced by over 4x, which seem to contain a lot of bot tags.
            // continue;
            ts_write(&key, ts);
            ts_putc(0, ts);
            ts_write(&val, ts);
            ts_putc(0, ts);
        } else if (code < 0) {
            // Negative code provides key lookup, but value is written as zero-terminated free text.
            ts_write(&val, ts);
            ts_putc(0, ts);
        }
        n_tags_written++;
    }
    /* If all tags were skipped, return the index of the shared zero-length list. */
    if (n_tags_written == 0) return 0;
    /* The tag list is terminated with a single character. TODO maybe use 0 as terminator. */
    ts_putc(INT8_MAX, ts);
    return position;
}

/* Count the number of nodes and ways loaded, just for progress reporting. */
static long nodes_loaded = 0;
static long ways_loaded = 0;

/* Node callback handed to the general-purpose PBF loading code. */
static void handle_node (OSMPBF__Node *node, ProtobufCBinaryData *string_table) {
    if (node->id > MAX_NODE_ID)
        die("OSM data contains nodes with larger IDs than expected.");
    if (ways_loaded > 0)
        die("All nodes must appear before any ways in input file.");
    double lat = node->lat * 0.0000001;
    double lon = node->lon * 0.0000001;
    to_coord(&(nodes[node->id].coord), lat, lon);
    TagSubfile *ts = tag_subfile_for_id(node->id, NODE);
    nodes[node->id].tags = write_tags (node->keys, node->vals, node->n_keys, string_table, ts);
    nodes_loaded++;
    if (nodes_loaded % 1000000 == 0)
        printf("loaded %ldM nodes\n", nodes_loaded / 1000000);
    //printf ("---\nlon=%.5f lat=%.5f\nx=%d y=%d\n", lon, lat, nodes[node->id].x, nodes[node->id].y);
}

/*
  Way callback handed to the general-purpose PBF loading code.
  All nodes must come before any ways in the input for this to work.
*/
static void handle_way (OSMPBF__Way *way, ProtobufCBinaryData *string_table) {
    if (way->id > MAX_WAY_ID)
        die("OSM data contains ways greater IDs than expected.");
    /*
       Copy node references into a sub-segment of one big array, reversing the PBF delta coding so
       they are absolute IDs. All the refs within a way or relation are always known at once, so
       we can use exact-length lists (unlike the lists of ways within a grid cell).
       Each way stores the index of the first node reference in its list, and a negative node
       ID is used to signal the end of the list.
    */
    ways[way->id].node_ref_offset = n_node_refs;
    //printf("WAY %ld\n", way->id);
    //printf("node ref offset %d\n", ways[way->id].node_ref_offset);
    int64_t node_id = 0;
    for (int r = 0; r < way->n_refs; r++, n_node_refs++) {
        node_id += way->refs[r]; // node refs are delta coded
        node_refs[n_node_refs] = node_id;
        if (n_node_refs == UINT32_MAX) die ("Node refs index is about to overflow.");
    }
    node_refs[n_node_refs - 1] *= -1; // Negate last node ref to signal end of list.
    /* Index this way, as being in the grid cell of its first node. */
    uint32_t wbi = get_grid_way_block(&(nodes[way->refs[0]]));
    WayBlock *wb = &(way_blocks[wbi]);
    /* If the last node ref is non-negative, no free slots remain. Chain a new empty block. */
    if (wb->refs[WAY_BLOCK_SIZE - 1] >= 0) {
        int32_t n_wbi = new_way_block();
        // Insert new block at head of list to avoid later scanning though large swaths of memory.
        wb = &(way_blocks[n_wbi]);
        wb->next = wbi;
        set_grid_way_block(&(nodes[way->refs[0]]), n_wbi);
    }
    /* We are now certain to have a free slot in the current block. */
    int nfree = wb->refs[WAY_BLOCK_SIZE - 1];
    if (nfree >= 0) die ("Final ref should be negative, indicating number of empty slots.");
    /* A final ref < 0 gives the number of free slots in this block. */
    int free_idx = WAY_BLOCK_SIZE + nfree;
    wb->refs[free_idx] = way->id;
    /* If this was not the last available slot, reduce number of free slots in this block by one. */
    if (nfree != -1) (wb->refs[WAY_BLOCK_SIZE - 1])++;
    ways_loaded++;
    /* Save tags to compacted tag array, and record the index where that tag list begins. */
    TagSubfile *ts = tag_subfile_for_id(way->id, WAY);
    ways[way->id].tags = write_tags (way->keys, way->vals, way->n_keys, string_table, ts);
    if (ways_loaded % 1000000 == 0) {
        printf("loaded %ldM ways\n", ways_loaded / 1000000);
    }
}

/*
  Used for setting the grid side empirically.
  With 8 bit (255x255) grid, planet.pbf gives 36.87% full
  With 14 bit grid: 248351486 empty 20083970 used, 7.48% full
*/
static void fillFactor () {
    int used = 0;
    for (int i = 0; i < GRID_DIM; ++i) {
        for (int j = 0; j < GRID_DIM; ++j) {
            if (grid->cells[i][j] != 0) used++;
        }
    }
    printf("index grid: %d used, %.2f%% full\n",
        used, ((double)used) / (GRID_DIM * GRID_DIM) * 100);
}

/* Print out a message explaining command line parameters to the user, then exit. */
static void usage () {
    printf("usage:\nvex database_dir input.osm.pbf\n");
    printf("vex database_dir min_lat min_lon max_lat max_lon\n");
    exit(EXIT_SUCCESS);
}

/* Range checking. */
static void check_lat_range(double lat) {
    if (lat < -90 && lat > 90)
        die ("Latitude out of range.");
}

/* Range checking. */
static void check_lon_range(double lon) {
    if (lon < -180 && lon > 180)
        die ("Longitude out of range.");
}

/* Functions beginning with print_ output OSM in a simple structured text format. */
static void print_tags (uint32_t idx) {
    if (idx == UINT32_MAX) return; // special index indicating no tags
    char *t = NULL; // &(tags[idx]); // FIXME segment
    KeyVal kv;
    while (*t != INT8_MAX) {
        t += decode_tag(t, &kv);
        printf("%s=%s ", kv.key, kv.val);
    }
}

static void print_node (uint64_t node_id) {
    Node node = nodes[node_id];
    printf("  node %ld (%.6f, %.6f) ", node_id, get_lat(&node.coord), get_lon(&node.coord));
    print_tags(nodes[node_id].tags);
    printf("\n");
}

static void print_way (int64_t way_id) {
    printf("way %ld ", way_id);
    print_tags(ways[way_id].tags);
    printf("\n");
}

/*
  Fields and functions for saving compact binary OSM.
  This is comparable in size to PBF if you zlib it in blocks, but much simpler.
*/
FILE *ofile;
int32_t last_x, last_y;
int64_t last_node_id, last_way_id;

static void save_init () {
    ofile = NULL; //open_db_file("out.bin", 0);
    last_x = 0;
    last_y = 0;
    last_node_id = 0;
    last_way_id = 0;
}

static void save_tags (uint32_t idx) {
    KeyVal kv;
    if (idx != UINT32_MAX) {
        char *t0 = NULL; //&(tags[idx]); // FIXME segment
        char *t = t0;
        while (*t != INT8_MAX) t += decode_tag(t, &kv);
        fwrite(t0, t - t0, 1, ofile);
    }
    fputc(INT8_MAX, ofile);
}

static void save_node (int64_t node_id) {
    Node node = nodes[node_id];
    uint8_t buf[10]; // 10 is max length of a 64 bit varint
    int64_t id_delta = node_id - last_node_id;
    int32_t x_delta = node.coord.x - last_x;
    int32_t y_delta = node.coord.y - last_y;
    size_t size;
    size = sint64_pack(id_delta, buf);
    fwrite(&buf, size, 1, ofile);
    size = sint32_pack(x_delta, buf);
    fwrite(&buf, size, 1, ofile);
    size = sint32_pack(y_delta, buf);
    fwrite(&buf, size, 1, ofile);
    save_tags(node.tags);
    last_node_id = node_id;
    last_x = node.coord.x;
    last_y = node.coord.y;
}

static void save_way (int64_t way_id) {
    Way way = ways[way_id];
    uint8_t buf[10]; // 10 is max length of a 64 bit varint
    int64_t id_delta = way_id - last_way_id;
    size_t size = sint64_pack(id_delta, buf);
    fwrite(&buf, size, 1, ofile);
    save_tags(way.tags);
    last_way_id = way_id;
}

int main (int argc, const char * argv[]) {

    if (argc != 3 && argc != 6) usage();
    database_path = argv[1];
    in_memory = (strcmp(database_path, "memory") == 0);
    lock_fd = open("/tmp/vex.lock", O_CREAT, S_IRWXU);
    if (lock_fd == -1) die ("Error opening or creating lock file.");

    /* Memory-map files for each OSM element type, and for references between them. */
    grid       = map_file("grid",       0, sizeof(Grid));
    ways       = map_file("ways",       0, sizeof(Way)      * MAX_WAY_ID);
    nodes      = map_file("nodes",      0, sizeof(Node)     * MAX_NODE_ID);
    node_refs  = map_file("node_refs",  0, sizeof(int64_t)  * MAX_NODE_REFS);
    way_blocks = map_file("way_blocks", 0, sizeof(WayBlock) * MAX_WAY_BLOCKS);

    if (argc == 3) {
        /* LOAD */
        const char *filename = argv[2];
        osm_callbacks_t callbacks;
        callbacks.way = &handle_way;
        callbacks.node = &handle_node;
        /* Request an exclusive write lock, blocking while reads complete. */
        printf("Acquiring exclusive write lock on database.\n");
        flock(lock_fd, LOCK_EX); 
        scan_pbf(filename, &callbacks); // we could just pass the callbacks by value
        fillFactor();
        /* Release exclusive write lock, allowing reads to begin. */
        flock(lock_fd, LOCK_UN);
        printf("loaded %ld nodes and %ld ways total.\n", nodes_loaded, ways_loaded);
        return EXIT_SUCCESS;
    } else if (argc == 6) {
        /* QUERY */
        double min_lat = strtod(argv[2], NULL);
        double min_lon = strtod(argv[3], NULL);
        double max_lat = strtod(argv[4], NULL);
        double max_lon = strtod(argv[5], NULL);
        printf("min = (%.5lf, %.5lf) max = (%.5lf, %.5lf)\n", min_lat, min_lon, max_lat, max_lon);
        check_lat_range(min_lat);
        check_lat_range(max_lat);
        check_lon_range(min_lon);
        check_lon_range(max_lon);
        if (min_lat >= max_lat) die ("min lat must be less than max lat.");
        if (min_lon >= max_lon) die ("min lon must be less than max lon.");
        coord_t cmin, cmax;
        to_coord(&cmin, min_lat, min_lon);
        to_coord(&cmax, max_lat, max_lon);
        uint32_t min_xbin = bin(cmin.x);
        uint32_t max_xbin = bin(cmax.x);
        uint32_t min_ybin = bin(cmin.y);
        uint32_t max_ybin = bin(cmax.y);

        /* Request a shared read lock, blocking while any writes to complete. */
        printf("Acquiring shared read lock on database.\n");
        flock(lock_fd, LOCK_SH); 
        FILE *pbf_file = open_output_file("out.pbf", 0);
        write_pbf_begin(pbf_file);

        /* Make two passes, first outputting all nodes, then all ways. */
        for (int stage = NODE; stage < RELATION; stage++) {
            for (uint32_t x = min_xbin; x <= max_xbin; x++) {
                for (uint32_t y = min_ybin; y <= max_ybin; y++) {
                    uint32_t wbidx = grid->cells[x][y];
                    // printf ("xbin=%d ybin=%d way bin index %u\n", x, y, wbidx);
                    if (wbidx == 0) continue; // There are no ways in this grid cell.
                    /* Iterate over all ways in this block, and its chained blocks. */
                    WayBlock *wb = &(way_blocks[wbidx]);
                    for (;;) {
                        for (int w = 0; w < WAY_BLOCK_SIZE; w++) {
                            int64_t way_id = wb->refs[w];
                            if (way_id <= 0) break;
                            Way way = ways[way_id];
                            if (stage == WAY) {
                                uint8_t *tags = tag_data_for_id(way_id, WAY);
                                write_pbf_way(way_id, &(node_refs[way.node_ref_offset]), &(tags[way.tags]));
                            } else if (stage == NODE) {
                                /* Output all nodes in this way. */
                                //FIXME Intersection nodes are repeated.
                                uint32_t nr = way.node_ref_offset;
                                bool more = true;
                                for (; more; nr++) {
                                    int64_t node_id = node_refs[nr];
                                    if (node_id < 0) {
                                        node_id = -node_id;
                                        more = false;
                                    }
                                    Node node = nodes[node_id];
                                    uint8_t *tags = tag_data_for_id(node_id, NODE);
                                    write_pbf_node(node_id, get_lat(&(node.coord)),
                                        get_lon(&(node.coord)), &(tags[node.tags]));
                                }
                            }
                        }
                        if (wb->next == 0) break;
                        wb = &(way_blocks[wb->next]);
                    }
                }
            }
            /* Write out any buffered nodes or ways before beginning the next PBF writing stage. */
            write_pbf_flush();
        }
        fclose(pbf_file);
        flock(lock_fd, LOCK_UN); // release the shared lock, allowing writes to begin.
    }

}

// TODO simple network protocol for fetching PBF.
