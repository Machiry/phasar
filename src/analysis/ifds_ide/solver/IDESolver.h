/******************************************************************************
 * Copyright (c) 2017 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

/*
 * IDESolver.h
 *
 *  Created on: 04.08.2016
 *      Author: pdschbrt
 */

#ifndef ANALYSIS_IFDS_IDE_SOLVER_IDESOLVER_H_
#define ANALYSIS_IFDS_IDE_SOLVER_IDESOLVER_H_

#include "../../../lib/LLVMShorthands.h"
#include "../../../utils/Logger.h"
#include "../../../utils/Table.h"
#include "../EdgeFunction.h"
#include "../EdgeFunctions.h"
#include "../FlowEdgeFunctionCache.h"
#include "../FlowFunctions.h"
#include "../IDETabulationProblem.h"
#include "../JoinLattice.h"
#include "../ZeroedFlowFunction.h"
#include "../edge_func/EdgeIdentity.h"
#include "../solver/JumpFunctions.h"
#include "IFDSToIDETabulationProblem.h"
#include "JoinHandlingNode.h"
#include "LinkedNode.h"
#include "PathEdge.h"
#include "json.hpp"
#include <chrono>
#include <curl/curl.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <set>
#include <stdio.h>
#include <sys/stat.h>
#include <type_traits>
#include <utility>
#include <iostream>
#include <fstream>
#include <string>

using json = nlohmann::json;
using namespace std;

// Forward declare the Transformation
template <typename N, typename D, typename M, typename I>
class IFDSToIDETabulationProblem;

/**
 * Solves the given IDETabulationProblem as described in the 1996 paper by
 * Sagiv, Horwitz and Reps. To solve the problem, call solve(). Results
 * can then be queried by using resultAt() and resultsAt().
 *
 * @param <N> The type of nodes in the interprocedural control-flow graph.
 * @param <D> The type of data-flow facts to be computed by the tabulation
 * problem.
 * @param <M> The type of objects used to represent methods.
 * @param <V> The type of values to be computed along flow edges.
 * @param <I> The type of inter-procedural control-flow graph being used.
 */
template <typename N, typename D, typename M, typename V, typename I>
class IDESolver
{
public:
  IDESolver(IDETabulationProblem<N, D, M, V, I> &tabulationProblem)
      : ideTabulationProblem(tabulationProblem),
        cachedFlowEdgeFunctions(tabulationProblem),
        recordEdges(tabulationProblem.solver_config.recordEdges),
        zeroValue(tabulationProblem.zeroValue()),
        icfg(tabulationProblem.interproceduralCFG()),
        computevalues(tabulationProblem.solver_config.computeValues),
        autoAddZero(tabulationProblem.solver_config.autoAddZero),
        followReturnPastSeeds(
            tabulationProblem.solver_config.followReturnsPastSeeds),
        computePersistedSummaries(
            tabulationProblem.solver_config.computePersistedSummaries),
        allTop(tabulationProblem.allTopFunction()),
        jumpFn(make_shared<JumpFunctions<N, D, V>>(allTop)),
        initialSeeds(tabulationProblem.initialSeeds())
  {
    cout << "called IDESolver::IDESolver() ctor with IDEProblem" << endl;
  }

  virtual ~IDESolver() = default;

  unordered_set<string> methodSet;
  unordered_set<string> stmtSet;
  json graph;

  void sendGraphToServer()
  {

    ofstream o("myJsonGraph.json");
    o << graph << endl;
    o.close();

    CURL *curl;
    CURLcode res;

    struct curl_httppost *formpost = NULL;
    struct curl_httppost *lastptr = NULL;
    struct curl_slist *headerlist = NULL;
    static const char buf[] = "Expect:";

    curl_global_init(CURL_GLOBAL_ALL);

    /* Fill in the file upload field */
    curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "sendfile",
                 CURLFORM_FILE, "myJsonGraph.json", CURLFORM_END);

    /* Fill in the filename field */
    curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "filename",
                 CURLFORM_COPYCONTENTS, "myJsonGraph.json", CURLFORM_END);

    /* Fill in the submit field too, even if this is rarely needed */
    curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "submit",
                 CURLFORM_COPYCONTENTS, "send", CURLFORM_END);

    curl = curl_easy_init();
    /* initalize custom header list (stating that Expect: 100-continue is not
     wanted */
    headerlist = curl_slist_append(headerlist, buf);
    if (curl)
    {
      /* what URL that receives this POST */
      curl_easy_setopt(curl, CURLOPT_URL,
                       "http://localhost:3000/api/framework/addGraph");
      // if ( (argc == 2) && (!strcmp(argv[1], "noexpectheader")) )
      //   /* only disable 100-continue header if explicitly requested */
      //   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist);
      curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);

      /* Perform the request, res will get the return code */
      res = curl_easy_perform(curl);
      /* Check for errors */
      if (res != CURLE_OK)
        fprintf(stderr, "curl_easy_perform() failed: %s\n",
                curl_easy_strerror(res));

      /* always cleanup */
      curl_easy_cleanup(curl);

      /* then cleanup the formpost chain */
      curl_formfree(formpost);
      /* free slist */
      curl_slist_free_all(headerlist);
    }

    if (remove("myJsonGraph.json") != 0)
      cout << "Error deleting file" << endl;
    else
      cout << "File successfully deleted" << endl;
  }

  void exportJson(string graphId)
  {
    cout << "new export for graph " << graphId << endl;
    vector<json> methods;
    vector<json> statements;
    vector<json> dataflowfacts;

    graph = {{"id", graphId},
             {"methods", methods},
             {"statements", statements},
             {"dataflowFacts", dataflowfacts}};

    for (auto seed : initialSeeds)
    {
      graph["methods"].push_back(
          {{"methodName",
            ideTabulationProblem.MtoString(icfg.getMethodOf(seed.first))}});
      iterateMethod(icfg.getSuccsOf(seed.first));
    }

    sendGraphToServer();
  }

  json getStatementJson(N succ)
  {
    auto currentId = icfg.getStatementId(succ);
    auto currentMethodName =
        ideTabulationProblem.MtoString(icfg.getMethodOf(succ));
    auto content = ideTabulationProblem.NtoString(succ);

    auto dVMap = resultsAt(succ);
    vector<string> dffIds;
    int i = 0;
    for (auto it : dVMap)
    {
      string dfId = currentId + "_dff_" + to_string(i);
      i++;
      json dfFact = {{"id", dfId},
                     {"content", ideTabulationProblem.DtoString(it.first)},
                     {"value", ideTabulationProblem.VtoString(it.second)},
                     {"statementId", currentId},
                     {"type", 5}};
      dffIds.push_back(dfId);
      graph["dataflowFacts"].push_back(dfFact);
    }

    auto next = icfg.getSuccsOf(succ);
    vector<string> succIds;

    for (auto stmt : next)
    {
      succIds.push_back(icfg.getStatementId(stmt));
    }

    json statement = {{"id", currentId}, {"method", currentMethodName}, {"content", content}, {"successors", succIds}, {"dataflowFacts", dffIds}, {"type", 0}};
    return statement;
  }

  void iterateMethod(vector<N> succs)
  {
    for (auto succ : succs)
    {
      auto currentId = icfg.getStatementId(succ);
      if (stmtSet.find(currentId) == stmtSet.end())
      {
        stmtSet.insert(currentId);
        json statement = getStatementJson(succ);

        if (icfg.isCallStmt(succ))
        {
          // if statement is call statement create call and returnsite
          // connect statement with callsite, callsite with returnsite,
          // returnsite with return statement annotate callsite and returnsite
          // with method name (name is unique)

          // called methods
          auto calledMethods = icfg.getCalleesOfCallAt(succ);
          vector<string> targetMethods;
          statement["type"] = 1;
          for (auto method : calledMethods)
          {
            auto methodName = ideTabulationProblem.MtoString(method);
            statement["successors"].push_back(methodName);

            targetMethods.push_back(methodName);
            if (methodSet.find(methodName) == methodSet.end())
            {
              graph["methods"].push_back({{"methodName", methodName}});
              // start points of called method
              auto nodeSet = icfg.getStartPointsOf(method);
              for (auto tmp : nodeSet)
              {
                methodSet.insert(methodName);
                iterateMethod(icfg.getSuccsOf(tmp));
              }
            }
          }
          statement["targetMethods"] = targetMethods;
          auto returnsites = icfg.getReturnSitesOfCallAt(succ);

          for (auto returnsite : returnsites)
          {
            auto returnsiteId = icfg.getStatementId(returnsite);

            json returnsiteStmt = getStatementJson(returnsite);
            returnsiteStmt["type"] = 2;
            for (auto m : targetMethods)
            {
              returnsiteStmt["successors"].push_back(m);
            }

            graph["statements"].push_back(returnsiteStmt);
            stmtSet.insert(returnsiteId);
            iterateMethod(icfg.getSuccsOf(returnsite));
          }
        }

        graph["statements"].push_back(statement);
        iterateMethod(icfg.getSuccsOf(succ));
      }
    }
  }
  /**
   * @brief Runs the solver on the configured problem. This can take some time.
   */
  virtual void solve()
  {
    PAMM_FACTORY;
    REG_COUNTER("FF Construction");
    REG_COUNTER("FF Application");
    REG_COUNTER("SpecialSummary-FF Application");
    REG_COUNTER("Propagation");
    REG_COUNTER("Calls to processCall");
    REG_COUNTER("Calls to processNormal");
    REG_COUNTER("Calls to getPointsToSet");
    REG_SETH("Data-flow facts");
    REG_SETH("IDESolver");
    REG_SETH("Points-to");
    auto &lg = lg::get();
    BOOST_LOG_SEV(lg, INFO) << "IDE solver is solving the specified problem";
    // computations starting here
    START_TIMER("DFA FF-Construction");
    // We start our analysis and construct exploded supergraph
    BOOST_LOG_SEV(lg, INFO)
        << "Submit initial seeds, construct exploded super graph";
    submitInitalSeeds();
    STOP_TIMER("DFA FF-Construction");
    if (computevalues)
    {
      START_TIMER("DFA FF-Application");
      // Computing the final values for the edge functions
      BOOST_LOG_SEV(lg, INFO)
          << "Compute the final values according to the edge functions";
      computeValues();
      STOP_TIMER("DFA FF-Application");
    }
    BOOST_LOG_SEV(lg, INFO) << "Problem solved";
#ifdef PERFORMANCE_EVA
    BOOST_LOG_SEV(lg, INFO) << "----------------------------------------------";
    BOOST_LOG_SEV(lg, INFO) << "Solver Statistics:";
    BOOST_LOG_SEV(lg, INFO) << "flow functions construction count: "
                            << GET_COUNTER("FF Construction");
    BOOST_LOG_SEV(lg, INFO) << "flow functions application count: "
                            << GET_COUNTER("FF Application");
    BOOST_LOG_SEV(lg, INFO) << "special flow function usage count: "
                            << GET_COUNTER("SpecialSummary-FF Application");
    BOOST_LOG_SEV(lg, INFO)
        << "propagation count: " << GET_COUNTER("Propagation");
    BOOST_LOG_SEV(lg, INFO) << "flow function construction duration: "
                            << PRINT_TIMER("FF Construction");
    BOOST_LOG_SEV(lg, INFO) << "flow function application duration: "
                            << PRINT_TIMER("FF Application");
    BOOST_LOG_SEV(lg, INFO) << "call count of process call function: "
                            << GET_COUNTER("Calls to processCall");
    BOOST_LOG_SEV(lg, INFO) << "call count of process normal function: "
                            << GET_COUNTER("Calls to processNormal");
    BOOST_LOG_SEV(lg, INFO) << "----------------------------------------------";
    cachedFlowEdgeFunctions.print();
#endif
  }

  /**
   * Returns the V-type result for the given value at the given statement.
   * TOP values are never returned.
   */
  V resultAt(N stmt, D value) { return valtab.get(stmt, value); }

  /**
   * Returns the resulting environment for the given statement.
   * The artificial zero value can be automatically stripped.
   * TOP values are never returned.
   */
  unordered_map<D, V> resultsAt(N stmt, bool stripZero = false)
  {
    unordered_map<D, V> result = valtab.row(stmt);
    if (stripZero)
    {
      for (auto &pair : result)
      {
        if (ideTabulationProblem.isZeroValue(pair.first))
          result.erase(pair.first);
      }
    }
    return result;
  }

private:
  unique_ptr<IFDSToIDETabulationProblem<N, D, M, I>> transformedProblem;
  IDETabulationProblem<N, D, M, V, I> &ideTabulationProblem;
  FlowEdgeFunctionCache<N, D, M, V, I> cachedFlowEdgeFunctions;
  bool recordEdges;

  void saveEdges(N sourceNode, N sinkStmt, D sourceVal, set<D> destVals,
                 bool interP)
  {
    PAMM_FACTORY;
    ADD_TO_SETH("Data-flow facts", destVals.size());
    if (!recordEdges)
      return;
    Table<N, N, map<D, set<D>>> &tgtMap =
        (interP) ? computedInterPathEdges : computedIntraPathEdges;
    tgtMap.get(sourceNode, sinkStmt)[sourceVal].insert(destVals.begin(),
                                                       destVals.end());
  }

  /**
   * Lines 13-20 of the algorithm; processing a call site in the caller's
   * context.
   *
   * For each possible callee, registers incoming call edges.
   * Also propagates call-to-return flows and summarized callee flows within the
   * caller.
   *
   * 	The following cases must be considered and handled:
   *		1. Process as usual and just process the call
   *		2. Create a new summary for that function (which shall be done
   *       by the problem)
   *		3. Just use an existing summary provided by the problem
   *		4. If a special function is called, use a special summary
   *       function
   *
   * @param edge an edge whose target node resembles a method call
   */
  void processCall(PathEdge<N, D> edge)
  {
    PAMM_FACTORY;
    INC_COUNTER("Calls to processCall");
    auto &lg = lg::get();
    BOOST_LOG_SEV(lg, DEBUG)
        << "process call at target: "
        << ideTabulationProblem.NtoString(edge.getTarget());
    D d1 = edge.factAtSource();
    N n = edge.getTarget(); // a call node; line 14...
    D d2 = edge.factAtTarget();
    shared_ptr<EdgeFunction<V>> f = jumpFunction(edge);
    set<N> returnSiteNs = icfg.getReturnSitesOfCallAt(n);
    ADD_TO_SETH("IDESolver", returnSiteNs.size());
    set<M> callees = icfg.getCalleesOfCallAt(n);
    ADD_TO_SETH("IDESolver", callees.size());
    BOOST_LOG_SEV(lg, DEBUG) << "possible callees:";
    for (auto callee : callees)
    {
      BOOST_LOG_SEV(lg, DEBUG) << callee->getName().str();
    }
    BOOST_LOG_SEV(lg, DEBUG) << "possible return sites:";
    for (auto ret : returnSiteNs)
    {
      BOOST_LOG_SEV(lg, DEBUG) << ideTabulationProblem.NtoString(ret);
    }
    // for each possible callee
    for (M sCalledProcN : callees)
    { // still line 14
      // check if a special summary for the called procedure exists
      shared_ptr<FlowFunction<D>> specialSum =
          cachedFlowEdgeFunctions.getSummaryFlowFunction(n, sCalledProcN);
      // if a special summary is available, treat this as a normal flow
      // and use the summary flow and edge functions
      if (specialSum)
      {
        BOOST_LOG_SEV(lg, DEBUG) << "Found and process special summary";
        for (N returnSiteN : returnSiteNs)
        {
          INC_COUNTER("SpecialSummary-FF Application");
          set<D> res = computeSummaryFlowFunction(specialSum, d1, d2);
          ADD_TO_SETH("Data-flow facts", res.size());
          saveEdges(n, returnSiteN, d2, res, false);
          for (D d3 : res)
          {
            shared_ptr<EdgeFunction<V>> sumEdgFnE =
                cachedFlowEdgeFunctions.getSummaryEdgeFunction(n, d2,
                                                               returnSiteN, d3);
            propagate(d1, returnSiteN, d3, f->composeWith(sumEdgFnE), n, false);
          }
        }
      }
      else
      {
        // compute the call-flow function
        shared_ptr<FlowFunction<D>> function =
            cachedFlowEdgeFunctions.getCallFlowFunction(n, sCalledProcN);
        INC_COUNTER("FF Construction");
        set<D> res = computeCallFlowFunction(function, d1, d2);
        ADD_TO_SETH("Data-flow facts", res.size());
        // for each callee's start point(s)
        set<N> startPointsOf = icfg.getStartPointsOf(sCalledProcN);
        ADD_TO_SETH("IDESolver", startPointsOf.size());
        if (startPointsOf.empty())
        {
          BOOST_LOG_SEV(lg, DEBUG) << "Start points of '" +
                                          icfg.getMethodName(sCalledProcN) +
                                          "' currently not available!";
        }
        // if startPointsOf is empty, the called function is a declaration
        for (N sP : startPointsOf)
        {
          saveEdges(n, sP, d2, res, true);
          // for each result node of the call-flow function
          for (D d3 : res)
          {
            // create initial self-loop
            propagate(d3, sP, d3, EdgeIdentity<V>::v(), n, false); // line 15
            // register the fact that <sp,d3> has an incoming edge from <n,d2>
            // line 15.1 of Naeem/Lhotak/Rodriguez
            addIncoming(sP, d3, n, d2);
            // line 15.2, copy to avoid concurrent modification exceptions by
            // other threads
            set<typename Table<N, D, shared_ptr<EdgeFunction<V>>>::Cell>
                endSumm = set<
                    typename Table<N, D, shared_ptr<EdgeFunction<V>>>::Cell>(
                    endSummary(sP, d3));
            ADD_TO_SETH("IDESolver", endSumm.size());
            // cout << "ENDSUMM" << endl;
            // sP->dump();
            // d3->dump();
            // printEndSummaryTab();
            // still line 15.2 of Naeem/Lhotak/Rodriguez
            // for each already-queried exit value <eP,d4> reachable from
            // <sP,d3>, create new caller-side jump functions to the return
            // sites because we have observed a potentially new incoming
            // edge into <sP,d3>
            for (typename Table<N, D, shared_ptr<EdgeFunction<V>>>::Cell entry :
                 endSumm)
            {
              N eP = entry.getRowKey();
              D d4 = entry.getColumnKey();
              shared_ptr<EdgeFunction<V>> fCalleeSummary = entry.getValue();
              // for each return site
              for (N retSiteN : returnSiteNs)
              {
                // compute return-flow function
                shared_ptr<FlowFunction<D>> retFunction =
                    cachedFlowEdgeFunctions.getRetFlowFunction(n, sCalledProcN,
                                                               eP, retSiteN);
                INC_COUNTER("FF Construction");
                set<D> returnedFacts = computeReturnFlowFunction(
                    retFunction, d3, d4, n, set<D>{d2});
                ADD_TO_SETH("Data-flow facts", returnedFacts.size());
                saveEdges(eP, retSiteN, d4, returnedFacts, true);
                // for each target value of the function
                for (D d5 : returnedFacts)
                {
                  // update the caller-side summary function
                  // get call edge function
                  shared_ptr<EdgeFunction<V>> f4 =
                      cachedFlowEdgeFunctions.getCallEdgeFunction(
                          n, d2, sCalledProcN, d3);
                  // get return edge function
                  shared_ptr<EdgeFunction<V>> f5 =
                      cachedFlowEdgeFunctions.getReturnEdgeFunction(
                          n, sCalledProcN, eP, d4, retSiteN, d5);
                  // compose call * calleeSummary * return edge functions
                  shared_ptr<EdgeFunction<V>> fPrime =
                      f4->composeWith(fCalleeSummary)->composeWith(f5);
                  D d5_restoredCtx = restoreContextOnReturnedFact(n, d2, d5);
                  // prpagte the effects of the entire call
                  propagate(d1, retSiteN, d5_restoredCtx,
                            f->composeWith(fPrime), n, false);
                }
              }
            }
          }
        }
      }
      // line 17-19 of Naeem/Lhotak/Rodriguez
      // process intra-procedural flows along call-to-return flow functions
      for (N returnSiteN : returnSiteNs)
      {
        shared_ptr<FlowFunction<D>> callToReturnFlowFunction =
            cachedFlowEdgeFunctions.getCallToRetFlowFunction(n, returnSiteN);
        INC_COUNTER("FF Construction");
        set<D> returnFacts =
            computeCallToReturnFlowFunction(callToReturnFlowFunction, d1, d2);
        ADD_TO_SETH("Data-flow facts", returnFacts.size());
        saveEdges(n, returnSiteN, d2, returnFacts, false);
        for (D d3 : returnFacts)
        {
          shared_ptr<EdgeFunction<V>> edgeFnE =
              cachedFlowEdgeFunctions.getCallToReturnEdgeFunction(
                  n, d2, returnSiteN, d3);
          propagate(d1, returnSiteN, d3, f->composeWith(edgeFnE), n, false);
        }
      }
    }
  }

  /**
   * Lines 33-37 of the algorithm.
   * Simply propagate normal, intra-procedural flows.
   * @param edge
   */
  void processNormalFlow(PathEdge<N, D> edge)
  {
    PAMM_FACTORY;
    INC_COUNTER("Calls to processNormal");
    auto &lg = lg::get();
    BOOST_LOG_SEV(lg, DEBUG)
        << "process normal at target: "
        << ideTabulationProblem.NtoString(edge.getTarget());
    if (edge.factAtSource() == nullptr)
      BOOST_LOG_SEV(lg, DEBUG) << "fact at source is nullptr";
    D d1 = edge.factAtSource();
    N n = edge.getTarget();
    D d2 = edge.factAtTarget();
    shared_ptr<EdgeFunction<V>> f = jumpFunction(edge);
    auto successorInst = icfg.getSuccsOf(n);
    for (auto m : successorInst)
    {
      shared_ptr<FlowFunction<D>> flowFunction =
          cachedFlowEdgeFunctions.getNormalFlowFunction(n, m);
      INC_COUNTER("FF Construction");
      set<D> res = computeNormalFlowFunction(flowFunction, d1, d2);
      ADD_TO_SETH("Data-flow facts", res.size());
      saveEdges(n, m, d2, res, false);
      for (D d3 : res)
      {
        shared_ptr<EdgeFunction<V>> fprime = f->composeWith(
            cachedFlowEdgeFunctions.getNormalEdgeFunction(n, d2, m, d3));
        propagate(d1, m, d3, fprime, nullptr, false);
      }
    }
  }

  void propagateValueAtStart(pair<N, D> nAndD, N n)
  {
    PAMM_FACTORY;
    D d = nAndD.second;
    M p = icfg.getMethodOf(n);
    for (N c : icfg.getCallsFromWithin(p))
    {
      for (auto entry : jumpFn->forwardLookup(d, c))
      {
        D dPrime = entry.first;
        shared_ptr<EdgeFunction<V>> fPrime = entry.second;
        N sP = n;
        V value = val(sP, d);
        propagateValue(c, dPrime, fPrime->computeTarget(value));
        INC_COUNTER("FF Application");
      }
    }
  }

  void propagateValueAtCall(pair<N, D> nAndD, N n)
  {
    PAMM_FACTORY;
    D d = nAndD.second;
    for (M q : icfg.getCalleesOfCallAt(n))
    {
      shared_ptr<FlowFunction<D>> callFlowFunction =
          cachedFlowEdgeFunctions.getCallFlowFunction(n, q);
      INC_COUNTER("FF Construction");
      for (D dPrime : callFlowFunction->computeTargets(d))
      {
        shared_ptr<EdgeFunction<V>> edgeFn =
            cachedFlowEdgeFunctions.getCallEdgeFunction(n, d, q, dPrime);
        for (N startPoint : icfg.getStartPointsOf(q))
        {
          propagateValue(startPoint, dPrime, edgeFn->computeTarget(val(n, d)));
          INC_COUNTER("FF Application");
        }
      }
    }
  }

  void propagateValue(N nHashN, D nHashD, V v)
  {
    V valNHash = val(nHashN, nHashD);
    V vPrime = joinValueAt(nHashN, nHashD, valNHash, v);
    if (!(vPrime == valNHash))
    {
      setVal(nHashN, nHashD, vPrime);
      valuePropagationTask(pair<N, D>(nHashN, nHashD));
    }
  }

  V val(N nHashN, D nHashD)
  {
    if (valtab.contains(nHashN, nHashD))
    {
      return valtab.get(nHashN, nHashD);
    }
    else
    {
      // implicitly initialized to top; see line [1] of Fig. 7 in SRH96 paper
      return ideTabulationProblem.topElement();
    }
  }

  void setVal(N nHashN, D nHashD, V l)
  {
    auto &lg = lg::get();
    // TOP is the implicit default value which we do not need to store.
    if (l == ideTabulationProblem.topElement())
    {
      // do not store top values
      valtab.remove(nHashN, nHashD);
    }
    else
    {
      valtab.insert(nHashN, nHashD, l);
    }
    BOOST_LOG_SEV(lg, DEBUG)
        << "VALUE: " << icfg.getMethodOf(nHashN)->getName().str() << " "
        << "node: " << ideTabulationProblem.NtoString(nHashN) << " "
        << "fact: " << ideTabulationProblem.DtoString(nHashD) << " "
        << "val: " << ideTabulationProblem.VtoString(l);
  }

  shared_ptr<EdgeFunction<V>> jumpFunction(PathEdge<N, D> edge)
  {
    if (!jumpFn->forwardLookup(edge.factAtSource(), edge.getTarget())
             .count(edge.factAtTarget()))
    {
      // JumpFn initialized to all-top, see line [2] in SRH96 paper
      return allTop;
    }
    return jumpFn->forwardLookup(edge.factAtSource(),
                                 edge.getTarget())[edge.factAtTarget()];
  }

  void addEndSummary(N sP, D d1, N eP, D d2, shared_ptr<EdgeFunction<V>> f)
  {
    // note: at this point we don't need to join with a potential previous f
    // because f is a jump function, which is already properly joined
    // within propagate(..)
    endsummarytab.get(sP, d1).insert(eP, d2, f);
  }

  // should be made a callable at some point
  void pathEdgeProcessingTask(PathEdge<N, D> edge)
  {
    PAMM_FACTORY;
    auto &lg = lg::get();
    INC_COUNTER("Propagation");
    BOOST_LOG_SEV(lg, DEBUG)
        << "Process path edge: <"
        << "D source: " << ideTabulationProblem.DtoString(edge.factAtSource())
        << ", "
        << "N target: " << ideTabulationProblem.NtoString(edge.getTarget())
        << ", "
        << "D target: " << ideTabulationProblem.DtoString(edge.factAtTarget())
        << ">";
    bool isCall = icfg.isCallStmt(edge.getTarget());
    if (!isCall)
    {
      if (icfg.isExitStmt(edge.getTarget()))
      {
        processExit(edge);
      }
      if (!icfg.getSuccsOf(edge.getTarget()).empty())
      {
        processNormalFlow(edge);
      }
    }
    else
    {
      processCall(edge);
    }
  }

  // should be made a callable at some point
  void valuePropagationTask(pair<N, D> nAndD)
  {
    N n = nAndD.first;
    // our initial seeds are not necessarily method-start points but here they
    // should be treated as such the same also for unbalanced return sites in
    // an unbalanced problem
    if (icfg.isStartPoint(n) || initialSeeds.count(n) ||
        unbalancedRetSites.count(n))
    {
      propagateValueAtStart(nAndD, n);
    }
    if (icfg.isCallStmt(n))
    {
      propagateValueAtCall(nAndD, n);
    }
  }

  // should be made a callable at some point
  void valueComputationTask(vector<N> values)
  {
    PAMM_FACTORY;
    for (N n : values)
    {
      for (N sP : icfg.getStartPointsOf(icfg.getMethodOf(n)))
      {
        Table<D, D, shared_ptr<EdgeFunction<V>>> lookupByTarget;
        lookupByTarget = jumpFn->lookupByTarget(n);
        for (typename Table<D, D, shared_ptr<EdgeFunction<V>>>::Cell
                 sourceValTargetValAndFunction : lookupByTarget.cellSet())
        {
          D dPrime = sourceValTargetValAndFunction.getRowKey();
          D d = sourceValTargetValAndFunction.getColumnKey();
          shared_ptr<EdgeFunction<V>> fPrime =
              sourceValTargetValAndFunction.getValue();
          V targetVal = val(sP, dPrime);
          setVal(n, d,
                 ideTabulationProblem.join(val(n, d),
                                           fPrime->computeTarget(targetVal)));
          INC_COUNTER("FF Application");
        }
      }
    }
  }

protected:
  D zeroValue;
  I icfg;
  bool computevalues;
  bool autoAddZero;
  bool followReturnPastSeeds;
  bool computePersistedSummaries;

  Table<N, N, map<D, set<D>>> computedIntraPathEdges;

  Table<N, N, map<D, set<D>>> computedInterPathEdges;

  shared_ptr<EdgeFunction<V>> allTop;

  shared_ptr<JumpFunctions<N, D, V>> jumpFn;

  // stores summaries that were queried before they were computed
  // see CC 2010 paper by Naeem, Lhotak and Rodriguez
  Table<N, D, Table<N, D, shared_ptr<EdgeFunction<V>>>> endsummarytab;

  // edges going along calls
  // see CC 2010 paper by Naeem, Lhotak and Rodriguez
  Table<N, D, map<N, set<D>>> incomingtab;

  // stores the return sites (inside callers) to which we have unbalanced
  // returns if followReturnPastSeeds is enabled
  set<N> unbalancedRetSites;

  map<N, set<D>> initialSeeds;

  Table<N, D, V> valtab;

  // When transforming an IFDSTabulationProblem into an IDETabulationProblem,
  // we need to allocate dynamically, otherwise the objects lifetime runs out -
  // as a modifiable r-value reference created here that should be stored in a
  // modifiable l-value reference within the IDESolver implementation leads to
  // (massive) undefined behavior (and nightmares):
  // https://stackoverflow.com/questions/34240794/understanding-the-warning-binding-r-value-to-l-value-reference
  IDESolver(IFDSTabulationProblem<N, D, M, I> &tabulationProblem)
      : transformedProblem(
            std::make_unique<IFDSToIDETabulationProblem<N, D, M, I>>(
                tabulationProblem)),
        ideTabulationProblem(*transformedProblem),
        cachedFlowEdgeFunctions(ideTabulationProblem),
        recordEdges(ideTabulationProblem.solver_config.recordEdges),
        zeroValue(ideTabulationProblem.zeroValue()),
        icfg(ideTabulationProblem.interproceduralCFG()),
        computevalues(ideTabulationProblem.solver_config.computeValues),
        autoAddZero(ideTabulationProblem.solver_config.autoAddZero),
        followReturnPastSeeds(
            ideTabulationProblem.solver_config.followReturnsPastSeeds),
        computePersistedSummaries(
            ideTabulationProblem.solver_config.computePersistedSummaries),
        allTop(ideTabulationProblem.allTopFunction()),
        jumpFn(make_shared<JumpFunctions<N, D, V>>(allTop)),
        initialSeeds(ideTabulationProblem.initialSeeds())
  {

    cout << "called IDESolver::IDESolver() ctor with IFDSProblem" << endl;
  }

  /**
   * Computes the final values for edge functions.
   */
  void computeValues()
  {
    PAMM_FACTORY;
    auto &lg = lg::get();
    BOOST_LOG_SEV(lg, DEBUG) << "start computing values";
    // Phase II(i)
    map<N, set<D>> allSeeds(initialSeeds);
    for (N unbalancedRetSite : unbalancedRetSites)
    {
      if (allSeeds[unbalancedRetSite].empty())
      {
        allSeeds.insert(make_pair(unbalancedRetSite, set<D>({zeroValue})));
        ADD_TO_SETH("Data-flow facts", 1);
      }
    }
    // do processing
    for (const auto &seed : allSeeds)
    {
      N startPoint = seed.first;
      for (D val : seed.second)
      {
        setVal(startPoint, val, ideTabulationProblem.bottomElement());
        pair<N, D> superGraphNode(startPoint, val);
        valuePropagationTask(superGraphNode);
      }
    }
    // Phase II(ii)
    // we create an array of all nodes and then dispatch fractions of this array
    // to multiple threads
    set<N> allNonCallStartNodes = icfg.allNonCallStartNodes();
    ADD_TO_SETH("IDESolver", allNonCallStartNodes.size());
    vector<N> nonCallStartNodesArray(allNonCallStartNodes.size());
    size_t i = 0;
    for (N n : allNonCallStartNodes)
    {
      nonCallStartNodesArray[i] = n;
      i++;
    }
    valueComputationTask(nonCallStartNodesArray);
  }

  /**
   * Schedules the processing of initial seeds, initiating the analysis.
   * Clients should only call this methods if performing synchronization on
   * their own. Normally, solve() should be called instead.
   */
  void submitInitalSeeds()
  {
    cout << "IDESolver::submitInitialSeeds()" << endl;
    for (const auto &seed : initialSeeds)
    {
      N startPoint = seed.first;
      cout << "submitInitialSeeds - Start point:" << endl;
      startPoint->dump();
      for (const D &value : seed.second)
      {
        cout << "submitInitialSeeds - Value:" << endl;
        value->dump();
        propagate(zeroValue, startPoint, value, EdgeIdentity<V>::v(), nullptr,
                  false);
      }
      jumpFn->addFunction(zeroValue, startPoint, zeroValue,
                          EdgeIdentity<V>::v());
    }
  }

  /**
   * Lines 21-32 of the algorithm.
   *
   * Stores callee-side summaries.
   * Also, at the side of the caller, propagates intra-procedural flows to
   * return sites using those newly computed summaries.
   *
   * @param edge an edge whose target node resembles a method exit
   */
  void processExit(PathEdge<N, D> edge)
  {
    PAMM_FACTORY;
    auto &lg = lg::get();
    BOOST_LOG_SEV(lg, DEBUG)
        << "process exit at target: "
        << ideTabulationProblem.NtoString(edge.getTarget());
    N n = edge.getTarget(); // an exit node; line 21...
    shared_ptr<EdgeFunction<V>> f = jumpFunction(edge);
    M methodThatNeedsSummary = icfg.getMethodOf(n);
    D d1 = edge.factAtSource();
    D d2 = edge.factAtTarget();
    // for each of the method's start points, determine incoming calls
    set<N> startPointsOf = icfg.getStartPointsOf(methodThatNeedsSummary);
    ADD_TO_SETH("IDESolver", startPointsOf.size());
    map<N, set<D>> inc;
    for (N sP : startPointsOf)
    {
      // line 21.1 of Naeem/Lhotak/Rodriguez
      // register end-summary
      addEndSummary(sP, d1, n, d2, f);
      for (auto entry : incoming(d1, sP))
      {
        inc[entry.first] = set<D>{entry.second};
        ADD_TO_SETH("Data-flow facts", inc[entry.first].size());
      }
    }
    printEndSummaryTab();
    printIncomingTab();
    // for each incoming call edge already processed
    //(see processCall(..))
    for (auto entry : inc)
    {
      // line 22
      N c = entry.first;
      // for each return site
      for (N retSiteC : icfg.getReturnSitesOfCallAt(c))
      {
        // compute return-flow function
        shared_ptr<FlowFunction<D>> retFunction =
            cachedFlowEdgeFunctions.getRetFlowFunction(
                c, methodThatNeedsSummary, n, retSiteC);
        INC_COUNTER("FF Construction");
        // for each incoming-call value
        for (D d4 : entry.second)
        {
          set<D> targets =
              computeReturnFlowFunction(retFunction, d1, d2, c, entry.second);
          ADD_TO_SETH("Data-flow facts", targets.size());
          saveEdges(n, retSiteC, d2, targets, true);
          // for each target value at the return site
          // line 23
          for (D d5 : targets)
          {
            // compute composed function
            // get call edge function
            shared_ptr<EdgeFunction<V>> f4 =
                cachedFlowEdgeFunctions.getCallEdgeFunction(
                    c, d4, icfg.getMethodOf(n), d1);
            // get return edge function
            shared_ptr<EdgeFunction<V>> f5 =
                cachedFlowEdgeFunctions.getReturnEdgeFunction(
                    c, icfg.getMethodOf(n), n, d2, retSiteC, d5);
            // compose call function * function * return function
            shared_ptr<EdgeFunction<V>> fPrime =
                f4->composeWith(f)->composeWith(f5);
            // for each jump function coming into the call, propagate to return
            // site using the composed function
            for (auto valAndFunc : jumpFn->reverseLookup(c, d4))
            {
              shared_ptr<EdgeFunction<V>> f3 = valAndFunc.second;
              if (!f3->equalTo(allTop))
              {
                D d3 = valAndFunc.first;
                D d5_restoredCtx = restoreContextOnReturnedFact(c, d4, d5);
                propagate(d3, retSiteC, d5_restoredCtx, f3->composeWith(fPrime),
                          c, false);
              }
            }
          }
        }
      }
    }
    // handling for unbalanced problems where we return out of a method with a
    // fact for which we have no incoming flow.
    // note: we propagate that way only values that originate from ZERO, as
    // conditionally generated values should only
    // be propagated into callers that have an incoming edge for this condition
    if (followReturnPastSeeds && inc.empty() &&
        ideTabulationProblem.isZeroValue(d1))
    {
      set<N> callers = icfg.getCallersOf(methodThatNeedsSummary);
      ADD_TO_SETH("IDESolver", callers.size());
      for (N c : callers)
      {
        for (N retSiteC : icfg.getReturnSitesOfCallAt(c))
        {
          shared_ptr<FlowFunction<D>> retFunction =
              cachedFlowEdgeFunctions.getRetFlowFunction(
                  c, methodThatNeedsSummary, n, retSiteC);
          INC_COUNTER("FF Construction");
          set<D> targets = computeReturnFlowFunction(retFunction, d1, d2, c,
                                                     set<D>{zeroValue});
          ADD_TO_SETH("Data-flow facts", targets.size());
          saveEdges(n, retSiteC, d2, targets, true);
          for (D d5 : targets)
          {
            shared_ptr<EdgeFunction<V>> f5 =
                cachedFlowEdgeFunctions.getReturnEdgeFunction(
                    c, icfg.getMethodOf(n), n, d2, retSiteC, d5);
            propagteUnbalancedReturnFlow(retSiteC, d5, f->composeWith(f5), c);
            // register for value processing (2nd IDE phase)
            unbalancedRetSites.insert(retSiteC);
          }
        }
      }
      // in cases where there are no callers, the return statement would
      // normally not be processed at all; this might be undesirable if
      // the flow function has a side effect such as registering a taint;
      // instead we thus call the return flow function will a null caller
      if (callers.empty())
      {
        shared_ptr<FlowFunction<D>> retFunction =
            cachedFlowEdgeFunctions.getRetFlowFunction(
                nullptr, methodThatNeedsSummary, n, nullptr);
        INC_COUNTER("FF Construction");
        retFunction->computeTargets(d2);
      }
    }
  }

  void propagteUnbalancedReturnFlow(N retSiteC, D targetVal,
                                    shared_ptr<EdgeFunction<V>> edgeFunction,
                                    N relatedCallSite)
  {
    propagate(zeroValue, retSiteC, targetVal, edgeFunction, relatedCallSite,
              true);
  }

  /**
   * This method will be called for each incoming edge and can be used to
   * transfer knowledge from the calling edge to the returning edge, without
   * affecting the summary edges at the callee.
   * @param callSite
   *
   * @param d4
   *            Fact stored with the incoming edge, i.e., present at the
   *            caller side
   * @param d5
   *            Fact that originally should be propagated to the caller.
   * @return Fact that will be propagated to the caller.
   */
  D restoreContextOnReturnedFact(N callSite, D d4, D d5)
  {
    // TODO support LinkedNode and JoinHandlingNode
    //		if (d5 instanceof LinkedNode) {
    //			((LinkedNode<D>) d5).setCallingContext(d4);
    //		}
    //		if(d5 instanceof JoinHandlingNode) {
    //			((JoinHandlingNode<D>) d5).setCallingContext(d4);
    //		}
    return d5;
  }

  /**
   * Computes the normal flow function for the given set of start and end
   * abstractions-
   * @param flowFunction The normal flow function to compute
   * @param d1 The abstraction at the method's start node
   * @param d2 The abstraction at the current node
   * @return The set of abstractions at the successor node
   */
  set<D> computeNormalFlowFunction(shared_ptr<FlowFunction<D>> flowFunction,
                                   D d1, D d2)
  {
    return flowFunction->computeTargets(d2);
  }

  /**
   * TODO: comment
   */
  set<D>
  computeSummaryFlowFunction(shared_ptr<FlowFunction<D>> SummaryFlowFunction,
                             D d1, D d2)
  {
    return SummaryFlowFunction->computeTargets(d2);
  }

  /**
   * Computes the call flow function for the given call-site abstraction
   * @param callFlowFunction The call flow function to compute
   * @param d1 The abstraction at the current method's start node.
   * @param d2 The abstraction at the call site
   * @return The set of caller-side abstractions at the callee's start node
   */
  set<D> computeCallFlowFunction(shared_ptr<FlowFunction<D>> callFlowFunction,
                                 D d1, D d2)
  {
    return callFlowFunction->computeTargets(d2);
  }

  /**
   * Computes the call-to-return flow function for the given call-site
   * abstraction
   * @param callToReturnFlowFunction The call-to-return flow function to
   * compute
   * @param d1 The abstraction at the current method's start node.
   * @param d2 The abstraction at the call site
   * @return The set of caller-side abstractions at the return site
   */
  set<D> computeCallToReturnFlowFunction(
      shared_ptr<FlowFunction<D>> callToReturnFlowFunction, D d1, D d2)
  {
    return callToReturnFlowFunction->computeTargets(d2);
  }

  /**
   * Computes the return flow function for the given set of caller-side
   * abstractions.
   * @param retFunction The return flow function to compute
   * @param d1 The abstraction at the beginning of the callee
   * @param d2 The abstraction at the exit node in the callee
   * @param callSite The call site
   * @param callerSideDs The abstractions at the call site
   * @return The set of caller-side abstractions at the return site
   */
  set<D> computeReturnFlowFunction(shared_ptr<FlowFunction<D>> retFunction,
                                   D d1, D d2, N callSite,
                                   set<D> callerSideDs)
  {
    return retFunction->computeTargets(d2);
  }

  /**
   * Propagates the flow further down the exploded super graph, merging any edge
   * function that might already have been computed for targetVal at
   * target.
   *
   * @param sourceVal the source value of the propagated summary edge
   * @param target the target statement
   * @param targetVal the target value at the target statement
   * @param f the new edge function computed from (s0,sourceVal) to
   * (target,targetVal)
   * @param relatedCallSite for call and return flows the related call
   * statement, nullptr otherwise (this value is not used within this
   * implementation but may be useful for subclasses of IDESolver)
   * @param isUnbalancedReturn true if this edge is propagating an
   * unbalanced return (this value is not used within this implementation
   * but may be useful for subclasses of {@link IDESolver})
   */
  void
  propagate(D sourceVal, N target, D targetVal, shared_ptr<EdgeFunction<V>> f,
            /* deliberately exposed to clients */ N relatedCallSite,
            /* deliberately exposed to clients */ bool isUnbalancedReturn)
  {
    auto &lg = lg::get();
    shared_ptr<EdgeFunction<V>> jumpFnE = nullptr;
    shared_ptr<EdgeFunction<V>> fPrime;
    if (!jumpFn->reverseLookup(target, targetVal).empty())
    {
      jumpFnE = jumpFn->reverseLookup(target, targetVal)[sourceVal];
    }
    if (jumpFnE == nullptr)
    {
      jumpFnE = allTop; // jump function is initialized to all-top
    }
    fPrime = jumpFnE->joinWith(f);
    bool newFunction = !(fPrime->equalTo(jumpFnE));
    if (newFunction)
    {
      jumpFn->addFunction(sourceVal, target, targetVal, fPrime);
      PathEdge<N, D> edge(sourceVal, target, targetVal);
      pathEdgeProcessingTask(edge);
      if (!ideTabulationProblem.isZeroValue(targetVal))
      {
        BOOST_LOG_SEV(lg, DEBUG)
            << "EDGE: <F: " << target->getFunction()->getName().str()
            << ", D: " << ideTabulationProblem.DtoString(sourceVal)
            << "> ---> <N: " << ideTabulationProblem.NtoString(target)
            << ", D: " << ideTabulationProblem.DtoString(targetVal) << ">";
      }
    }
  }

  V joinValueAt(N unit, D fact, V curr, V newVal)
  {
    return ideTabulationProblem.join(curr, newVal);
  }

  set<typename Table<N, D, shared_ptr<EdgeFunction<V>>>::Cell>
  endSummary(N sP, D d3)
  {
    return endsummarytab.get(sP, d3).cellSet();
  }

  map<N, set<D>> incoming(D d1, N sP) { return incomingtab.get(sP, d1); }

  void addIncoming(N sP, D d3, N n, D d2)
  {
    incomingtab.get(sP, d3)[n].insert(d2);
  }

  void printIncomingTab()
  {
    auto &lg = lg::get();
    BOOST_LOG_SEV(lg, DEBUG) << "start incomingtab entry";
    for (auto cell : incomingtab.cellSet())
    {
      BOOST_LOG_SEV(lg, DEBUG)
          << "sP: " << ideTabulationProblem.NtoString(cell.r);
      BOOST_LOG_SEV(lg, DEBUG)
          << "d3: " << ideTabulationProblem.DtoString(cell.c);
      for (auto entry : cell.v)
      {
        BOOST_LOG_SEV(lg, DEBUG)
            << "n: " << ideTabulationProblem.NtoString(entry.first);
        for (auto fact : entry.second)
        {
          BOOST_LOG_SEV(lg, DEBUG)
              << "d2: " << ideTabulationProblem.DtoString(fact);
        }
      }
      BOOST_LOG_SEV(lg, DEBUG) << "-----";
    }
    BOOST_LOG_SEV(lg, DEBUG) << "end incomingtab entry";
  }

  void printEndSummaryTab()
  {
    auto &lg = lg::get();
    BOOST_LOG_SEV(lg, DEBUG) << "start endsummarytab entry";
    for (auto cell : endsummarytab.cellVec())
    {
      BOOST_LOG_SEV(lg, DEBUG)
          << "sP: " << ideTabulationProblem.NtoString(cell.r);
      BOOST_LOG_SEV(lg, DEBUG)
          << "d1: " << ideTabulationProblem.DtoString(cell.c);
      for (auto inner_cell : cell.v.cellVec())
      {
        BOOST_LOG_SEV(lg, DEBUG)
            << "eP: " << ideTabulationProblem.NtoString(inner_cell.r);
        BOOST_LOG_SEV(lg, DEBUG)
            << "d2: " << ideTabulationProblem.DtoString(inner_cell.c);
        BOOST_LOG_SEV(lg, DEBUG) << "edge fun: " << inner_cell.v->toString();
      }
      BOOST_LOG_SEV(lg, DEBUG) << "-----";
    }
    BOOST_LOG_SEV(lg, DEBUG) << "end endsummarytab entry";
  }
};

#endif /* ANALYSIS_IFDS_IDE_SOLVER_IDESOLVER_HH_ */
