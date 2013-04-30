/** Count triangles -*- C++ -*-
 * @file
 *
 * Count the number of triangles in a graph.
 *
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @author Donald Nguyen <ddn@cs.utexas.edu>
 */
#include "Galois/Galois.h"
#include "Galois/Accumulator.h"
#include "Galois/Bag.h"
#include "Galois/Statistic.h"
#include "Galois/Graph/LCGraph.h"
#include "Galois/ParallelSTL/ParallelSTL.h"
#include "llvm/Support/CommandLine.h"
#include "Lonestar/BoilerPlate.h"
#include "Galois/Runtime/Network.h"
#include "Galois/Graphs/Graph3.h"
#include "Galois/Runtime/DistSupport.h"

#include <boost/iterator/transform_iterator.hpp>
#include <Eigen/Dense>
#include <utility>
#include <vector>
#include <algorithm>
#include <iostream>

const char* name = "Triangles";
const char* desc = "Count triangles in a graph";
const char* url = 0;

enum Algo {
  nodeiterator,
};

namespace cll = llvm::cl;
static cll::opt<std::string> inputFilename(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<Algo> algo("algo", cll::desc("Choose an algorithm:"),
    cll::values(
      clEnumValN(Algo::nodeiterator, "nodeiterator", "Node Iterator (default)"),
      clEnumValEnd), cll::init(Algo::nodeiterator));

typedef Galois::Graph::LC_Numa_Graph<uint32_t,void> Graph;
typedef Graph::GraphNode GNode;
Graph graph;

// DistGraph nodes
typedef Galois::Graph::ThirdGraph<uint32_t,void,Galois::Graph::EdgeDirection::Un> DGraph;
typedef DGraph::NodeHandle DGNode;
typedef typename DGraph::pointer Graphp;

std::unordered_map<GNode,DGNode> mapping;

struct element: public Galois::Runtime::Lockable {
  GNode g;
  unsigned v;
  element() { }
  element(GNode _g,unsigned _v): g(_g), v(_v) { }
  // serialization functions
  typedef int tt_has_serialize;
  void serialize(Galois::Runtime::Distributed::SerializeBuffer& s) const {
    gSerialize(s,g,v);
  }
  void deserialize(Galois::Runtime::Distributed::DeSerializeBuffer& s) {
    gDeserialize(s,g,v);
  }
};

/**
 * Like std::lower_bound but doesn't dereference iterators. Returns the first element
 * for which comp is not true. 
 */
template<typename Iterator, typename Compare>
Iterator lowerBound(Iterator first, Iterator last, Compare comp) {
  Iterator it;
  typename std::iterator_traits<Iterator>::difference_type count, half;
  count = std::distance(first, last);
  while (count > 0) {
    it = first; half = count / 2; std::advance(it, half);
    if (comp(it)) {
      first = ++it;
      count -= half + 1;
    } else {
      count = half;
    }
  }
  return first;
}

/**
 * std::set_intersection over edge_iterators.
 */
template<typename G>
struct LessThan {
  Graphp& g;
  DGNode n;
  LessThan(Graphp& g, DGNode n): g(g), n(n) { }
  bool operator()(typename G::edge_iterator it) {
    return g->getEdgeDst(it) < n;
  }
};

template<typename G>
struct GreaterThanOrEqual {
  Graphp& g;
  DGNode n;
  GreaterThanOrEqual(Graphp& g, DGNode n): g(g), n(n) { }
  bool operator()(typename G::edge_iterator it) {
    return !(n < g->getEdgeDst(it));
  }
};

/**
 * Node Iterator algorithm for counting triangles.
 * <code>
 * for (v in G) 
 *   for (all pairs of neighbors (a, b) of v)
 *     if ((a,b) in G and a < v < b)
 *       triangle += 1
 * </code>
 *
 * Thomas Schank. Algorithmic Aspects of Triangle-Based Network Analysis. PhD
 * Thesis. Universitat Karlsruhe. 2007.
 */
struct NodeIteratorAlgo {
  Galois::GAccumulator<size_t> numTriangles;
  
  struct Process {
    Graphp g;
    NodeIteratorAlgo* self;
    Process(NodeIteratorAlgo* s,Graphp _g): g(_g), self(s) { }

    void operator()(const DGNode& n, Galois::UserContext<GNode>&) { (*this)(n); }
    void operator()(const DGNode& n) {
      // Partition neighbors
      // [first, ea) [n] [bb, last)
      DGraph::edge_iterator first = g->edge_begin(n);
      DGraph::edge_iterator last = g->edge_end(n);
      DGraph::edge_iterator ea = lowerBound(first, last, LessThan<DGraph>(g, n));
      DGraph::edge_iterator bb = lowerBound(first, last, GreaterThanOrEqual<DGraph>(g, n));

      for (; bb != last; ++bb) {
        DGNode B = g->getEdgeDst(bb);
        for (auto aa = first; aa != ea; ++aa) {
          DGNode A = g->getEdgeDst(aa);
          DGraph::edge_iterator vv = g->edge_begin(A);
          DGraph::edge_iterator ev = g->edge_end(A);
          DGraph::edge_iterator it = lowerBound(vv, ev, LessThan<DGraph>(g, B));
          if (it != ev && g->getEdgeDst(it) == B) {
            self->numTriangles += 1;
          }
        }
      }
    }
  };

  void operator()(Graphp g) { 
    Galois::do_all_local(g, Process(this,g));
    std::cout << "NumTriangles: " << numTriangles.reduce() << "\n";
  }
};

template<typename Algo>
void run() {
  Algo algo;

  Galois::StatTimer T;
  T.start();
  algo();
  T.stop();
}

using namespace Galois::Runtime;
using namespace Galois::Runtime::Distributed;

typedef  Galois::Runtime::LL::SimpleLock<true> SLock;

struct create_nodes {
  Graphp g;
  SLock& l;
  create_nodes(Graphp _g, SLock& _l): g(_g), l(_l) {}

  void operator()(GNode& item, Galois::UserContext<GNode>& ctx) {
    unsigned val = graph.getData(item,Galois::MethodFlag::NONE);
    DGNode n = g->createNode(val);
    g->addNode(n);
    l.lock();
    mapping[item] = n;
    l.unlock();
  }
};

static void create_dist_graph(Graphp dgraph, std::string triangleFilename) {
  SLock lk;
  uint64_t block, f, l;
  Graph::iterator first, last;
  std::unordered_map<GNode,unsigned> e;

  graph.structureFromFile(triangleFilename);
  unsigned size = 0;
  for (auto ii = graph.begin(); ii != graph.end(); ++ii) {
    e[*ii] = size;
    ++size;
  }
  block = size / networkHostNum;
  f = networkHostID * block;
  l = (networkHostID + 1) * block;
  first = graph.begin() + (networkHostID * block);
  last  = graph.begin() + ((networkHostID + 1) * block);
  if (networkHostID == (networkHostNum-1)) last = graph.end();
  // create the nodes
printf ("host: %u creating nodes\n", networkHostID);
  Galois::for_each(first,last,create_nodes(dgraph,lk));
  printf ("%lu nodes in %u host with block size %lu\n", mapping.size(), networkHostID, block);
  // create the local edges
printf ("host: %u creating edges\n", networkHostID);
unsigned count = 0;
unsigned scount = 0;
  for(auto ii = first; ii != last; ++ii) {
    Graph::edge_iterator vv = graph.edge_begin(*ii, Galois::MethodFlag::NONE);
    Graph::edge_iterator ev = graph.edge_end(*ii, Galois::MethodFlag::NONE);
scount++;
    for (Graph::edge_iterator jj = vv; jj != ev; ++jj) {
      unsigned num = e[graph.getEdgeDst(jj)];
      if ((f <= num) && (num < l)) {
        dgraph->addEdge(mapping[*ii],mapping[graph.getEdgeDst(jj)]);
count++;
      }
      else {
        printf("host %u - Edge to external node\n", networkHostID);
      }
    }
  }
printf("nodes %u and edges %u\n", scount, count);
printf ("host: %u done creating edges\n", networkHostID);
}

static void readInputGraph_landing_pad(Distributed::RecvBuffer& buf) {
  Graphp dgraph;
  std::string triangleFilename;
  Distributed::gDeserialize(buf, triangleFilename, dgraph);
printf("host: %u and thread id: %d\t %s\n", Distributed::networkHostID, LL::getTID(), triangleFilename.c_str());
  create_dist_graph(dgraph,triangleFilename);
}

void readInputGraph(Graphp dgraph, std::string triangleFilename) {
  if (Distributed::networkHostNum > 1) {
    Distributed::SendBuffer b;
    Distributed::gSerialize(b, triangleFilename, dgraph);
    Distributed::getSystemNetworkInterface().broadcast(readInputGraph_landing_pad, b);
    Distributed::getSystemNetworkInterface().handleReceives();
  }
  create_dist_graph(dgraph,triangleFilename);
}

void readGraph(Graphp dgraph) {
  if (inputFilename.find(".gr.triangles") != inputFilename.size() - strlen(".gr.triangles")) {
    // Not directly passed .gr.triangles file
    std::string triangleFilename = inputFilename + ".triangles";
    std::ifstream triangleFile(triangleFilename.c_str());
    if (!triangleFile.good()) {
      // triangles doesn't already exist, create it
      //makeGraph(triangleFilename);
      abort();
    } else {
      // triangles does exist, load it
      readInputGraph(dgraph, triangleFilename);
    }
  } else {
    //graph.structureFromFile(inputFilename);
    printf("No triangles file!\n");
    abort();
  }

  size_t index = 0;
  for (GNode n : graph) {
    graph.getData(n) = index++;
  }
}

int main(int argc, char** argv) {
  Galois::StatManager statManager;
  LonestarStart(argc, argv, name, desc, url);

  // check the host id and initialise the network
  Galois::Runtime::Distributed::networkStart();

  Graphp dgraph = DGraph::allocate();

  // XXX Test if preallocation matters
  Galois::Statistic("MeminfoPre", Galois::Runtime::MM::pageAllocInfo());
  Galois::preAlloc(numThreads + 8 * Galois::Runtime::MM::pageAllocInfo());
  Galois::Statistic("MeminfoMid", Galois::Runtime::MM::pageAllocInfo());

  Galois::StatTimer Tinitial("InitializeTime");
  Tinitial.start();
  readGraph(dgraph);
  Tinitial.stop();

  Galois::StatTimer T;
  T.start();
  NodeIteratorAlgo()(dgraph);
  T.stop();
  Galois::Statistic("MeminfoPost", Galois::Runtime::MM::pageAllocInfo());

  // TODO Print num triangles

  // master_terminate();
  Galois::Runtime::Distributed::networkTerminate();

  return 0;
}
