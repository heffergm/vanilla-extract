#!/usr/bin/python

# http://imposm.org/docs/imposm/latest/install.html
# sudo apt-get install build-essential python-dev protobuf-compiler libprotobuf-dev libtokyocabinet-dev python-psycopg2 libgeos-c1 python-pip
# sudo pip install imposm.parser
    
# NOTE that similar tag statistics functionality is available in 'osmfilter'
# which is part of the ubuntu package 'osmctools' (C language OSM tools).
# Also includes 'osmconvert' and 'osmchange'.
# https://gitorious.org/osm-c-tools/
# These are speedy tools that handle the PBF format among others.

from imposm.parser import OSMParser
import sys

skip_keys = ['name', 'note', 'operator', 'source', 'tiger:', 'nhd', 'zip', 'RLIS:', 'gnis', 'addr:', 'import', 'created', 'CCGIS', 'website']

retain_keys = ['building', 'highway', 'footway', 'cycleway', 'surface', 'railway', 'amenity', 'public_transport', 'bridge', 'embankment', 'tunnel', 'bicycle', 'oneway', 'natural', 'lanes', 'landuse', "RLIS:bicycle", "CCGIS:bicycle"]

def dump_tags(tags):
    weighted = []
    for (key, value), count in tags.iteritems():
        weight = (len(key) + len(value) + 2) * count
        weighted.append((key, value, count, weight))
    weighted.sort(key=lambda x: x[3], reverse=True)
    for num, tag in enumerate(weighted):
        print num, "%s=%s (%d)" % tag[:3]
        if num > 512:
            break

class TagCounter(object):
    way_tags = {}
    node_tags = {}
    rel_tags = {}
    key_weights = {}
    role_weights = {}
    n_nodes = 0
    n_ways = 0
    n_relations = 0
    
    def count_tags(self, target, tags):
        for tag in tags.iteritems():
            key, value = tag
            #if any(key.startswith(sk) for sk in skip_keys):
            if key not in retain_keys:
                continue
            if key not in self.key_weights:
                self.key_weights[key] = 0
            self.key_weights[key] += (len(key) + len(value) + 2)
            if tag not in target:
                target[tag] = 1
            target[tag] += 1

    def ways(self, ways):
        for osmid, tags, refs in ways:
            self.n_ways += 1
            if (self.n_ways % 10000 == 0):
                print "%d ways" % self.n_ways
            self.count_tags(self.way_tags, tags)            

    def nodes(self, nodes):
        for osmid, tags, coord in nodes:
            self.n_nodes += 1
            if (self.n_nodes % 10000 == 0):
                print "%d nodes" % self.n_nodes
            self.count_tags(self.node_tags, tags)            

    def relations(self, relations):
        for osmid, tags, members in relations:
            self.n_relations += 1
            if (self.n_relations % 1000 == 0):
                print "%d relations" % self.n_relations
            for ref, element_type, role in members:
                if role not in self.role_weights:
                    self.role_weights[role] = 0
                self.role_weights[role] += 1
            self.count_tags(self.rel_tags, tags)            

tc = TagCounter()
p = OSMParser(concurrency=1, ways_callback=tc.ways, nodes_callback=tc.nodes, relations_callback=tc.relations)
#p = OSMParser(concurrency=1, relations_callback=tc.relations)
p.parse(sys.argv[1])

print "KEY WEIGHTS"
worst = [x for x in tc.key_weights.iteritems()]
worst.sort(key = lambda x : x[1])
for key, weight in worst[-100:]:
    print key, weight
print "NODES"
dump_tags(tc.node_tags)
print "WAYS"
dump_tags(tc.way_tags)
print "AVOID"
print skip_keys

role_counts = tc.role_weights.items()
role_counts.sort(key=lambda x: x[1], reverse=True)
for role, count in role_counts :
    print role, count
    

