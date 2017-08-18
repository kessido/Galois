/** partitioned graph wrapper for cartesianCut -*- C++ -*-
 * @file
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
 * @section Contains the cartesian/grid vertex-cut functionality to be used in dGraph.
 *
 * @author Roshan Dathathri <roshan@cs.utexas.edu>
 */
#ifndef _GALOIS_DIST_HGRAPHCC_H
#define _GALOIS_DIST_HGRAPHCC_H

#include <vector>
#include <set>
#include <algorithm>
#include <unordered_map>
#include "Galois/Runtime/dGraph.h"
#include "Galois/Runtime/OfflineGraph.h"
#include "Galois/Runtime/Serialize.h"
#include "Galois/Runtime/Tracer.h"
#include "Galois/DoAllWrap.h"

template<typename NodeTy, typename EdgeTy, bool BSPNode = false, bool BSPEdge = false>
class hGraph_cartesianCut : public hGraph<NodeTy, EdgeTy, BSPNode, BSPEdge> {
public:
  typedef hGraph<NodeTy, EdgeTy, BSPNode, BSPEdge> base_hGraph;

private:
  unsigned numRowHosts;
  unsigned numColumnHosts;

  // factorize numHosts such that difference between factors is minimized
  void factorize_hosts() {
    numColumnHosts = sqrt(base_hGraph::numHosts);
    while ((base_hGraph::numHosts % numColumnHosts) != 0) numColumnHosts--;
    numRowHosts = base_hGraph::numHosts/numColumnHosts;
    assert(numRowHosts>=numColumnHosts);
    if (base_hGraph::id == 0) {
      std::cerr << "Cartesian grid: " << numRowHosts << " x " << numColumnHosts << "\n";
    }
  }

  unsigned gridRowID() const {
    return (base_hGraph::id / numColumnHosts);
  }

  unsigned gridRowID(unsigned id) const {
    return (id / numColumnHosts);
  }

  unsigned gridColumnID() const {
    return (base_hGraph::id % numColumnHosts);
  }

  unsigned gridColumnID(unsigned id) const {
    return (id % numColumnHosts);
  }

  unsigned getBlockID(uint64_t gid) const {
    return getHostID(gid);
  }

  unsigned getColumnHostIDOfBlock(uint32_t blockID) const {
    return (blockID % numColumnHosts); // round-robin, non-contiguous
  }

  unsigned getColumnHostID(uint64_t gid) const {
    assert(gid < base_hGraph::totalNodes);
    uint32_t blockID = getBlockID(gid);
    return getColumnHostIDOfBlock(blockID);
  }

  uint32_t getColumnIndex(uint64_t gid) const {
    assert(gid < base_hGraph::totalNodes);
    auto blockID = getBlockID(gid);
    auto h = getColumnHostIDOfBlock(blockID);
    uint32_t columnIndex = 0;
    for (auto b = 0U; b <= blockID; ++b) {
      if (getColumnHostIDOfBlock(b) == h) {
        uint64_t start, end;
        std::tie(start, end) = base_hGraph::gid2host[b];
        if (gid < end) {
          columnIndex += gid - start;
          break; // redundant
        } else {
          columnIndex += end - start;
        }
      }
    }
    return columnIndex;
  }

  // called only for those hosts with which it shares nodes
  bool isNotCommunicationPartner(unsigned host, typename base_hGraph::SyncType syncType, WriteLocation writeLocation, ReadLocation readLocation) {
    if (syncType == base_hGraph::syncReduce) {
      switch(writeLocation) {
        case writeSource:
          return (gridRowID() != gridRowID(host));
        case writeDestination:
          return (gridColumnID() != gridColumnID(host));
        case writeAny:
          assert((gridRowID() == gridRowID(host)) || (gridColumnID() == gridColumnID(host)));
          return ((gridRowID() != gridRowID(host)) && (gridColumnID() != gridColumnID(host))); // false
        default:
          assert(false);
      }
    } else { // syncBroadcast
      switch(readLocation) {
        case readSource:
          return (gridRowID() != gridRowID(host));
        case readDestination:
          return (gridColumnID() != gridColumnID(host));
        case readAny:
          assert((gridRowID() == gridRowID(host)) || (gridColumnID() == gridColumnID(host)));
          return ((gridRowID() != gridRowID(host)) && (gridColumnID() != gridColumnID(host))); // false
        default:
          assert(false);
      }
    }
    return false;
  }

public:
  // GID = localToGlobalVector[LID]
  std::vector<uint64_t> localToGlobalVector; // TODO use LargeArray instead
  // LID = globalToLocalMap[GID]
  std::unordered_map<uint64_t, uint32_t> globalToLocalMap;

  uint32_t numNodes;
  uint64_t numEdges;

  // Return the ID to which gid belongs after patition.
  unsigned getHostID(uint64_t gid) const {
    assert(gid < base_hGraph::totalNodes);
    for (auto h = 0U; h < base_hGraph::numHosts; ++h) {
      uint64_t start, end;
      std::tie(start, end) = base_hGraph::gid2host[h];
      if (gid >= start && gid < end) {
        return h;
      }
    }
    assert(false);
    return base_hGraph::numHosts;
  }

  // Return if gid is Owned by local host.
  bool isOwned(uint64_t gid) const {
    uint64_t start, end;
    std::tie(start, end) = base_hGraph::gid2host[base_hGraph::id];
    return gid >= start && gid < end;
  }

  // Return is gid is present locally (owned or mirror).
  virtual bool isLocal(uint64_t gid) const {
    assert(gid < base_hGraph::totalNodes);
    if (isOwned(gid)) return true;
    return (globalToLocalMap.find(gid) != globalToLocalMap.end());
  }

  virtual uint32_t G2L(uint64_t gid) const {
    assert(isLocal(gid));
    return globalToLocalMap.at(gid);
  }

  virtual uint64_t L2G(uint32_t lid) const {
    return localToGlobalVector[lid];
  }

  // requirement: for all X and Y,
  // On X, nothingToSend(Y) <=> On Y, nothingToRecv(X)
  // Note: templates may not be virtual, so passing types as arguments
  virtual bool nothingToSend(unsigned host, typename base_hGraph::SyncType syncType, WriteLocation writeLocation, ReadLocation readLocation) {
    auto &sharedNodes = (syncType == base_hGraph::syncReduce) ? base_hGraph::mirrorNodes : base_hGraph::masterNodes;
    if (sharedNodes[host].size() > 0) {
      return isNotCommunicationPartner(host, syncType, writeLocation, readLocation);
    }
    return true;
  }
  virtual bool nothingToRecv(unsigned host, typename base_hGraph::SyncType syncType, WriteLocation writeLocation, ReadLocation readLocation) {
    auto &sharedNodes = (syncType == base_hGraph::syncReduce) ? base_hGraph::masterNodes : base_hGraph::mirrorNodes;
    if (sharedNodes[host].size() > 0) {
      return isNotCommunicationPartner(host, syncType, writeLocation, readLocation);
    }
    return true;
  }

  /** 
   * Constructor for Cartesian Cut graph
   */
  hGraph_cartesianCut(const std::string& filename, 
              const std::string& partitionFolder, unsigned host, 
              unsigned _numHosts, std::vector<unsigned> scalefactor, 
              bool transpose = false) : base_hGraph(host, _numHosts) {
    if (transpose) {
      GALOIS_DIE("ERROR: transpose not supported for cartesian vertex-cuts");
    }

    Galois::Statistic statGhostNodes("TotalGhostNodes");
    Galois::StatTimer StatTimer_graph_construct("TIME_GRAPH_CONSTRUCT");
    StatTimer_graph_construct.start();
    Galois::StatTimer StatTimer_graph_construct_comm("TIME_GRAPH_CONSTRUCT_COMM");

    // only used to determine node splits among hosts; abandonded later
    // for the FileGraph which mmaps appropriate regions of memory
    Galois::Graph::OfflineGraph g(filename);

    base_hGraph::totalNodes = g.size();
    if (base_hGraph::id == 0) {
      std::cerr << "Total nodes : " << base_hGraph::totalNodes << "\n";
    }
    factorize_hosts();

    base_hGraph::computeMasters(g, scalefactor, false);

    // at this point gid2Host has pairs for how to split nodes among
    // hosts; pair has begin and end
    uint64_t nodeBegin = base_hGraph::gid2host[base_hGraph::id].first;
    typename Galois::Graph::OfflineGraph::edge_iterator edgeBegin = 
      g.edge_begin(nodeBegin);

    uint64_t nodeEnd = base_hGraph::gid2host[base_hGraph::id].second;
    typename Galois::Graph::OfflineGraph::edge_iterator edgeEnd = 
      g.edge_begin(nodeEnd);
    
    // file graph that is mmapped for much faster reading; will use this
    // when possible from now on in the code
    Galois::Graph::FileGraph fileGraph;

    fileGraph.partFromFile(filename,
      std::make_pair(boost::make_counting_iterator<uint64_t>(nodeBegin), 
                     boost::make_counting_iterator<uint64_t>(nodeEnd)),
      std::make_pair(edgeBegin, edgeEnd));

    std::vector<uint64_t> prefixSumOfEdges;
    loadStatistics(g, fileGraph, prefixSumOfEdges); // first pass of the graph file

    std::cerr << "[" << base_hGraph::id << "] Owned nodes: " << 
                 base_hGraph::totalOwnedNodes << "\n";

    std::cerr << "[" << base_hGraph::id << "] Ghost nodes: " << 
                 numNodes - base_hGraph::totalOwnedNodes << "\n";

    std::cerr << "[" << base_hGraph::id << "] Nodes which have edges: " << 
                 base_hGraph::numOwned << "\n";

    std::cerr << "[" << base_hGraph::id << "] Total edges : " << 
                 numEdges << "\n";

    base_hGraph::numNodes = numNodes;
    base_hGraph::numNodesWithEdges = base_hGraph::numOwned; // numOwned = #nodeswithedges
    base_hGraph::beginMaster = G2L(base_hGraph::gid2host[base_hGraph::id].first);
    base_hGraph::endMaster = G2L(base_hGraph::gid2host[base_hGraph::id].second - 1) + 1;

    if (numNodes > 0) {
      //assert(numEdges > 0);

      assert(prefixSumOfEdges.size() == numNodes);

      if (!edgeNuma) {
        base_hGraph::graph.allocateFrom(numNodes, numEdges);
      } else {
        printf("Edge based NUMA division on\n");
        base_hGraph::graph.allocateFrom(numNodes, numEdges, prefixSumOfEdges);
      }

      //std::cerr << "Allocate done\n";

      base_hGraph::graph.constructNodes();

      //std::cerr << "Construct nodes done\n";
      for (uint32_t n = 0; n < numNodes; ++n) {
        base_hGraph::graph.fixEndEdge(n, prefixSumOfEdges[n]);
      }
    }

    loadEdges(base_hGraph::graph, g, fileGraph); // second pass of the graph file
    std::cerr << "[" << base_hGraph::id << "] Edges loaded \n";

    fill_mirrorNodes(base_hGraph::mirrorNodes);

    // TODO revise how this works and make it consistent across cuts
    if (!edgeNuma) {
      Galois::StatTimer StatTimer_thread_ranges("TIME_THREAD_RANGES");
      StatTimer_thread_ranges.start();
      base_hGraph::determine_thread_ranges(numNodes, prefixSumOfEdges);
      StatTimer_thread_ranges.stop();
    }

    base_hGraph::determine_thread_ranges_master();
    base_hGraph::determine_thread_ranges_with_edges();
    base_hGraph::initialize_specific_ranges();

    StatTimer_graph_construct.stop();

    StatTimer_graph_construct_comm.start();
    base_hGraph::setup_communication();
    StatTimer_graph_construct_comm.stop();
  }

  void loadStatistics(Galois::Graph::OfflineGraph& g, 
                      Galois::Graph::FileGraph& fileGraph, 
                      std::vector<uint64_t>& prefixSumOfEdges) {
    base_hGraph::totalOwnedNodes = base_hGraph::gid2host[base_hGraph::id].second - base_hGraph::gid2host[base_hGraph::id].first;

    std::vector<Galois::DynamicBitSet> hasIncomingEdge(numColumnHosts);
    for (unsigned i = 0; i < numColumnHosts; ++i) {
      uint64_t columnBlockSize = 0;
      for (auto b = 0U; b < base_hGraph::numHosts; ++b) {
        if (getColumnHostIDOfBlock(b) == i) {
          uint64_t start, end;
          std::tie(start, end) = base_hGraph::gid2host[b];
          columnBlockSize += end - start;
        }
      }
      hasIncomingEdge[i].resize(columnBlockSize);
    }

    std::vector<std::vector<uint64_t> > numOutgoingEdges(numColumnHosts);
    for (unsigned i = 0; i < numColumnHosts; ++i) {
      numOutgoingEdges[i].assign(base_hGraph::totalOwnedNodes, 0);
    }
    uint64_t rowOffset = base_hGraph::gid2host[base_hGraph::id].first;

    Galois::Timer timer;
    timer.start();
    g.reset_seek_counters();
    auto ee = fileGraph.edge_begin(base_hGraph::gid2host[base_hGraph::id].first);
    for (auto src = base_hGraph::gid2host[base_hGraph::id].first; src < base_hGraph::gid2host[base_hGraph::id].second; ++src) {
      auto ii = ee;
      ee = fileGraph.edge_end(src);
      for (; ii < ee; ++ii) {
        auto dst = fileGraph.getEdgeDst(ii);
        auto h = getColumnHostID(dst);
        hasIncomingEdge[h].set(getColumnIndex(dst));
        numOutgoingEdges[h][src - rowOffset]++;
      }
    }
    timer.stop();
    fprintf(stderr, "[%u] Edge inspection time : %f seconds to read %lu bytes in %lu seeks\n", base_hGraph::id, timer.get_usec()/1000000.0f, g.num_bytes_read(), g.num_seeks());

    auto& net = Galois::Runtime::getSystemNetworkInterface();
    for (unsigned i = 0; i < numColumnHosts; ++i) {
      unsigned h = (gridRowID() * numColumnHosts) + i;
      if (h == base_hGraph::id) continue;
      Galois::Runtime::SendBuffer b;
      Galois::Runtime::gSerialize(b, numOutgoingEdges[i]);
      Galois::Runtime::gSerialize(b, hasIncomingEdge[i]);
      net.sendTagged(h, Galois::Runtime::evilPhase, b);
    }
    net.flush();

    for (unsigned i = 1; i < numColumnHosts; ++i) {
      decltype(net.recieveTagged(Galois::Runtime::evilPhase, nullptr)) p;
      do {
        net.handleReceives();
        p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
      } while (!p);
      unsigned h = (p->first % numColumnHosts);
      auto& b = p->second;
      Galois::Runtime::gDeserialize(b, numOutgoingEdges[h]);
      Galois::Runtime::gDeserialize(b, hasIncomingEdge[h]);
    }
    ++Galois::Runtime::evilPhase;

    for (unsigned i = 1; i < numColumnHosts; ++i) {
      hasIncomingEdge[0].bitwise_or(hasIncomingEdge[i]);
    }

    auto max_nodes = hasIncomingEdge[0].size();
    for (unsigned i = 0; i < numColumnHosts; ++i) {
      max_nodes += numOutgoingEdges[i].size();
    }
    localToGlobalVector.reserve(max_nodes);
    globalToLocalMap.reserve(max_nodes);
    prefixSumOfEdges.reserve(max_nodes);
    unsigned leaderHostID = gridRowID() * numColumnHosts;
    uint64_t src = base_hGraph::gid2host[leaderHostID].first;
    numNodes = 0;
    numEdges = 0;
    for (unsigned i = 0; i < numColumnHosts; ++i) {
      for (uint32_t j = 0; j < numOutgoingEdges[i].size(); ++j) {
        bool createNode = false;
        if (numOutgoingEdges[i][j] > 0) {
          createNode = true;
          numEdges += numOutgoingEdges[i][j];
        } else if (isOwned(src)) {
          createNode = true;
        } else if ((gridColumnID() == getColumnHostID(src)) 
          && hasIncomingEdge[0].test(getColumnIndex(src))) {
          assert(false); // should be owned
          fprintf(stderr, "WARNING: Partitioning of vertices resulted in some inconsistency");
          createNode = true;
        }
        if (createNode) {
          localToGlobalVector.push_back(src);
          globalToLocalMap[src] = numNodes++;
          prefixSumOfEdges.push_back(numEdges);
        }
        ++src;
      }
    }
    base_hGraph::numOwned = numNodes; // number of nodes for which there are outgoing edges
    for (unsigned i = 0; i < numRowHosts; ++i) {
      unsigned hostID = (i * numColumnHosts) + gridColumnID();
      if (hostID == base_hGraph::id) continue;
      uint64_t dst = base_hGraph::gid2host[hostID].first;
      uint64_t dst_end = base_hGraph::gid2host[hostID].second;
      for (; dst < dst_end; ++dst) {
        if (hasIncomingEdge[0].test(getColumnIndex(dst))) {
          localToGlobalVector.push_back(dst);
          globalToLocalMap[dst] = numNodes++;
          prefixSumOfEdges.push_back(numEdges);
        }
      }
    }
  }

  template<typename GraphTy>
  void loadEdges(GraphTy& graph, 
                 Galois::Graph::OfflineGraph& g,
                 Galois::Graph::FileGraph& fileGraph) {
    if (base_hGraph::id == 0) {
      if (std::is_void<typename GraphTy::edge_data_type>::value) {
        fprintf(stderr, "Loading void edge-data while creating edges.\n");
      } else {
        fprintf(stderr, "Loading edge-data while creating edges.\n");
      }
    }

    Galois::Timer timer;
    timer.start();
    g.reset_seek_counters();

    std::atomic<uint32_t> numNodesWithEdges;
    numNodesWithEdges = base_hGraph::totalOwnedNodes;
    Galois::on_each([&](unsigned tid, unsigned nthreads){
      if (tid == 0) loadEdgesFromFile(graph, g, fileGraph);
      // using multiple threads to receive is mostly slower and leads to a deadlock or hangs sometimes
      if ((nthreads == 1) || (tid == 1)) receiveEdges(graph, numNodesWithEdges);
    });
    ++Galois::Runtime::evilPhase;

    timer.stop();
    fprintf(stderr, "[%u] Edge loading time : %f seconds to read %lu bytes in %lu seeks\n", base_hGraph::id, timer.get_usec()/1000000.0f, g.num_bytes_read(), g.num_seeks());
  }

  template<typename GraphTy, typename std::enable_if<!std::is_void<typename GraphTy::edge_data_type>::value>::type* = nullptr>
  void loadEdgesFromFile(GraphTy& graph, 
                         Galois::Graph::OfflineGraph& g,
                         Galois::Graph::FileGraph& fileGraph) {
    unsigned h_offset = gridRowID() * numColumnHosts;
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    std::vector<std::vector<uint64_t>> gdst_vec(numColumnHosts);
    std::vector<std::vector<typename GraphTy::edge_data_type>> gdata_vec(numColumnHosts);

    auto ee = fileGraph.edge_begin(base_hGraph::gid2host[base_hGraph::id].first);
    for (auto n = base_hGraph::gid2host[base_hGraph::id].first; n < base_hGraph::gid2host[base_hGraph::id].second; ++n) {
      uint32_t lsrc = 0;
      uint64_t cur = 0;
      if (isLocal(n)) {
        lsrc = G2L(n);
        cur = *graph.edge_begin(lsrc, Galois::MethodFlag::UNPROTECTED);
      }
      auto ii = ee;
      ee = fileGraph.edge_end(n);
      for (unsigned i = 0; i < numColumnHosts; ++i) {
        gdst_vec[i].clear();
        gdata_vec[i].clear();
        gdst_vec[i].reserve(std::distance(ii, ee));
        gdata_vec[i].reserve(std::distance(ii, ee));
      }
      for (; ii < ee; ++ii) {
        uint64_t gdst = fileGraph.getEdgeDst(ii);
        auto gdata = fileGraph.getEdgeData<typename GraphTy::edge_data_type>(ii);
        int i = getColumnHostID(gdst);
        if ((h_offset + i) == base_hGraph::id) {
          assert(isLocal(n));
          uint32_t ldst = G2L(gdst);
          graph.constructEdge(cur++, ldst, gdata);
        } else {
          gdst_vec[i].push_back(gdst);
          gdata_vec[i].push_back(gdata);
        }
      }
      for (unsigned i = 0; i < numColumnHosts; ++i) {
        if (gdst_vec[i].size() > 0) {
          Galois::Runtime::SendBuffer b;
          Galois::Runtime::gSerialize(b, n);
          Galois::Runtime::gSerialize(b, gdst_vec[i]);
          Galois::Runtime::gSerialize(b, gdata_vec[i]);
          net.sendTagged(h_offset + i, Galois::Runtime::evilPhase, b);
        }
      }
      if (isLocal(n)) {
        assert(cur == (*graph.edge_end(lsrc)));
      }
    }
    net.flush();
  }

  template<typename GraphTy, typename std::enable_if<std::is_void<typename GraphTy::edge_data_type>::value>::type* = nullptr>
  void loadEdgesFromFile(GraphTy& graph, 
                         Galois::Graph::OfflineGraph& g,
                         Galois::Graph::FileGraph& fileGraph) {
    unsigned h_offset = gridRowID() * numColumnHosts;
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    std::vector<std::vector<uint64_t>> gdst_vec(numColumnHosts);

    auto ee = fileGraph.edge_begin(base_hGraph::gid2host[base_hGraph::id].first);
    for (auto n = base_hGraph::gid2host[base_hGraph::id].first; n < base_hGraph::gid2host[base_hGraph::id].second; ++n) {
      uint32_t lsrc = 0;
      uint64_t cur = 0;
      if (isLocal(n)) {
        lsrc = G2L(n);
        cur = *graph.edge_begin(lsrc, Galois::MethodFlag::UNPROTECTED);
      }
      auto ii = ee;
      ee = fileGraph.edge_end(n);
      for (unsigned i = 0; i < numColumnHosts; ++i) {
        gdst_vec[i].clear();
        gdst_vec[i].reserve(std::distance(ii, ee));
      }
      for (; ii < ee; ++ii) {
        uint64_t gdst = fileGraph.getEdgeDst(ii);
        int i = getColumnHostID(gdst);
        if ((h_offset + i) == base_hGraph::id) {
          assert(isLocal(n));
          uint32_t ldst = G2L(gdst);
          graph.constructEdge(cur++, ldst);
        } else {
          gdst_vec[i].push_back(gdst);
        }
      }
      for (unsigned i = 0; i < numColumnHosts; ++i) {
        if (gdst_vec[i].size() > 0) {
          Galois::Runtime::SendBuffer b;
          Galois::Runtime::gSerialize(b, n);
          Galois::Runtime::gSerialize(b, gdst_vec[i]);
          net.sendTagged(h_offset + i, Galois::Runtime::evilPhase, b);
        }
      }
      if (isLocal(n)) {
        assert(cur == (*graph.edge_end(lsrc)));
      }
    }
    net.flush();
  }

  template<typename GraphTy>
  void receiveEdges(GraphTy& graph, std::atomic<uint32_t>& numNodesWithEdges) {
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    while (numNodesWithEdges < base_hGraph::numOwned) {
      decltype(net.recieveTagged(Galois::Runtime::evilPhase, nullptr)) p;
      net.handleReceives();
      p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
      if (p) {
        auto& rb = p->second;
        uint64_t n;
        Galois::Runtime::gDeserialize(rb, n);
        std::vector<uint64_t> gdst_vec;
        Galois::Runtime::gDeserialize(rb, gdst_vec);
        assert(isLocal(n));
        uint32_t lsrc = G2L(n);
        uint64_t cur = *graph.edge_begin(lsrc, Galois::MethodFlag::UNPROTECTED);
        uint64_t cur_end = *graph.edge_end(lsrc);
        assert((cur_end - cur) == gdst_vec.size());
        deserializeEdges(graph, rb, gdst_vec, cur, cur_end);
        ++numNodesWithEdges;
      }
    }
  }

  template<typename GraphTy, typename std::enable_if<!std::is_void<typename GraphTy::edge_data_type>::value>::type* = nullptr>
  void deserializeEdges(GraphTy& graph, Galois::Runtime::RecvBuffer& b, 
      std::vector<uint64_t>& gdst_vec, uint64_t& cur, uint64_t& cur_end) {
    std::vector<typename GraphTy::edge_data_type> gdata_vec;
    Galois::Runtime::gDeserialize(b, gdata_vec);
    uint64_t i = 0;
    while (cur < cur_end) {
      auto gdata = gdata_vec[i];
      uint64_t gdst = gdst_vec[i++];
      uint32_t ldst = G2L(gdst);
      graph.constructEdge(cur++, ldst, gdata);
    }
  }

  template<typename GraphTy, typename std::enable_if<std::is_void<typename GraphTy::edge_data_type>::value>::type* = nullptr>
  void deserializeEdges(GraphTy& graph, Galois::Runtime::RecvBuffer& b, 
      std::vector<uint64_t>& gdst_vec, uint64_t& cur, uint64_t& cur_end) {
    uint64_t i = 0;
    while (cur < cur_end) {
      uint64_t gdst = gdst_vec[i++];
      uint32_t ldst = G2L(gdst);
      graph.constructEdge(cur++, ldst);
    }
  }

  void fill_mirrorNodes(std::vector<std::vector<size_t>>& mirrorNodes){
    // mirrors for outgoing edges
    for (unsigned i = 0; i < numColumnHosts; ++i) {
      unsigned hostID = (gridRowID() * numColumnHosts) + i;
      if (hostID == base_hGraph::id) continue;
      uint64_t src = base_hGraph::gid2host[hostID].first;
      uint64_t src_end = base_hGraph::gid2host[hostID].second;
      mirrorNodes[hostID].reserve(mirrorNodes[hostID].size() + src_end - src);
      for (; src < src_end; ++src) {
        if (globalToLocalMap.find(src) != globalToLocalMap.end()) {
          mirrorNodes[hostID].push_back(src);
        }
      }
    }
    // mirrors for incoming edges
    for (unsigned i = 0; i < numRowHosts; ++i) {
      unsigned hostID = (i * numColumnHosts) + gridColumnID();
      if (hostID == base_hGraph::id) continue;
      uint64_t dst = base_hGraph::gid2host[hostID].first;
      uint64_t dst_end = base_hGraph::gid2host[hostID].second;
      mirrorNodes[hostID].reserve(mirrorNodes[hostID].size() + dst_end - dst);
      for (; dst < dst_end; ++dst) {
        if (globalToLocalMap.find(dst) != globalToLocalMap.end()) {
          mirrorNodes[hostID].push_back(dst);
        }
      }
    }
  }

  std::string getPartitionFileName(const std::string& filename, const std::string & basename, unsigned hostID, unsigned num_hosts){
    return filename;
  }

  bool is_vertex_cut() const{
    if ((numRowHosts == 1) || (numColumnHosts == 1)) return false; // IEC or OEC
    return true;
  }

  /**
   * Returns the start and end of master nodes in local graph.
   */
  uint64_t get_local_total_nodes() const {
    return numNodes;
  }

  void reset_bitset(typename base_hGraph::SyncType syncType, 
                    void (*bitset_reset_range)(size_t, size_t)) const {
    size_t first_owned = G2L(base_hGraph::gid2host[base_hGraph::id].first);
    size_t last_owned = G2L(base_hGraph::gid2host[base_hGraph::id].second - 1);
    assert(first_owned <= last_owned);
    assert((last_owned - first_owned + 1) == base_hGraph::totalOwnedNodes);
    if (syncType == base_hGraph::syncBroadcast) { // reset masters
      bitset_reset_range(first_owned, last_owned);
    } else { // reset mirrors
      assert(syncType == base_hGraph::syncReduce);
      if (first_owned > 0) {
        bitset_reset_range(0, first_owned - 1);
      }
      if (last_owned < (numNodes - 1)) {
        bitset_reset_range(last_owned + 1, numNodes - 1);
      }
    }
  }
};
#endif
