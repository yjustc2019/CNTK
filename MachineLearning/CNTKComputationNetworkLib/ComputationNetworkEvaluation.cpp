//
// <copyright file="ComputationNetwork.cpp" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//

#define _CRT_SECURE_NO_WARNINGS // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings

#include "Basics.h"
#include "ComputationNode.h"
#include "ComputationNetwork.h"
#include "RecurrentNodes.h"
#include <string>
#include <vector>
#include <list>
#include <set>

using namespace std;

namespace Microsoft { namespace MSR { namespace CNTK {

    // This source file contains methods related to evaluation (forward prop, backprop) and network validation.

    // -----------------------------------------------------------------------
    // evaluation
    // -----------------------------------------------------------------------

    // MAIN ENTRY POINT for evaluating one minibatch (forward prop)
    // TODO: pass a set of nodes instead of only one
    // TODO: rename to ForwardProp()? To make it very clear?
    // This calls EvaluateThisNode() on all nodes in order of data flow through the network.
    // By default, the network is applied concurrently on all frames in a minibatch in parallel (a "map" operation)
    // Recurrent loops deviate:
    //  - a recurrent loop is the loop of nodes that make up computation for one time step (e.g. Times -> Plus -> Sigmoid -> Delay)
    //  - these must be executed frame by frame rather than as a map
    //  - such a loop is treated as if they were a little nested network; this is done inside here
    //  - these little nested networks are defined in m_recurrentInfo[]
    void ComputationNetwork::Evaluate(const ComputationNodeBasePtr & rootNode)
    {
        // caller must call BuildAndValidateSubNetwork() before
        // TODO: Some places are hard to fix, e.g. encoder-decoder best-path functions. Those may be broken; this message will tell you.
        if (!BuiltAndValidatedSubNetwork(rootNode))
            LogicError("Evaluate for node %ls %ls: BuildAndValidateSubNetwork() has not been called on this node.");

        // determines order of evaluation, such that children get evaluated before their parent nodes
        std::list<ComputationNodeBasePtr>& allNodes = GetEvalOrder(rootNode, false);

#ifdef DISPLAY_DEBUG
        for (auto nodeIter=allNodes.begin(); nodeIter != allNodes.end(); nodeIter++)
            fprintf (stderr, "Evaluate Node: %s\n",(msra::strfun::utf8 ((*nodeIter)->NodeName())).c_str());
#endif

        for (int i = 0; i < m_recurrentInfo.size(); i++)
            m_recurrentInfo[i].m_completedEvaluate = false;

        // traverse all nodes in the pre-determined evaluation order
        for (auto & node : allNodes)
        {
            // --- if this node is part of a recurrence, evaluate all nodes that participate in this loop

            RecurrentInfo * recInfo = FindInRecurrentLoops(node);   // check if this node participates in a recurrent loop

            if (recInfo && IsFuncValueOlderThanInputs(recInfo->m_recurrentNodesForForward) && !recInfo->m_completedEvaluate)
            {
                // node participates in a recurrent loop: process the loop frame by frame
                const auto & recurrentNodes = recInfo->m_recurrentNodesForForward;

                // get layout associated with this loop
                auto pMBLayout = recurrentNodes[0]->GetMBLayout();

                // tell all that loop is about to commence
                for (auto & node2 : recurrentNodes)
                {
                    if (!pMBLayout || node2->GetMBLayout() != pMBLayout)  // take the opportunity to check that layout is shared by all nodes in the loop
                        LogicError("Evaluate: all nodes inside a recurrent loop must have a layout that is identical; mismatch found for nodes '%ls' vs. '%ls'",
                                   node2->NodeName().c_str(), recurrentNodes[0]->NodeName().c_str());
                    node2->UpdateFunctionMBSize(); // TODO: for sequence-to-sequence models we will need to be able to grow this step by step since size is unknown upfront
                    node2->OnEvaluateBeginIteration();
                }

                //since we share memory we need to resize function value matrices correctly
                for (auto & node2 : recurrentNodes)
                {
                    //node2->UpdateFunctionMBSize();
                    node2->Validate(true);
                }

                // for every time step run through all nodes in this particular loop (treat the loop like a little ComputationNetwork)
                FrameRangeIteration range(pMBLayout, recInfo->m_steppingDirection);
                for (auto t = range.begin(); t != range.end(); t++)
                {
                    for (auto & node2 : recurrentNodes)
                    {
                        node2->EvaluateThisNode(t);
                        if (IsNodeReqMultiSeqHandling(node2))
                            node2->MaskMissingValuesColumnsToZero(t);
                        node2->UpdateEvalTimeStamp();
                    }
                } 

                // tell all that loop is done  --e.g. PastValueNode will capture its state for BPTT processing
                for (auto & node2 : recurrentNodes)
                    node2->OnEvaluateEndIteration();

                recInfo->m_completedEvaluate = true;
            }

            // --- not recurrent: do the whole batch (unless it's already done, e.g. because the node participated in a recurren ttloop)

            else if (!recInfo && node->IsFuncValueOlderThanInputs())
            {
#ifdef DISPLAY_DEBUG
                fprintf (stderr, "Evaluate Node: %s\n",(msra::strfun::utf8 (node->NodeName())).c_str());
#endif
#if DUMPOUTPUT
                fprintf(stderr,"Forward_%ls\n",node->NodeName().c_str());
#endif
                // evaluate the node for all frames concurrently (map)
                // we manage time stamp here so that derived classes don't need to worry about it
                node->UpdateFunctionMBSize();
                if (!node->IsLeaf() && !node->RequiresPreCompute())
                    node->Validate(true);
                node->OnEvaluateBeginIteration();
                node->EvaluateThisNode(FrameRange(node->GetMBLayout()));
                if (IsNodeReqMultiSeqHandling(node))
                    node->MaskMissingValuesColumnsToZero(FrameRange(node->GetMBLayout()));
                node->OnEvaluateEndIteration();
                node->UpdateEvalTimeStamp();
            }
            else
                node->OnEvaluateEndIteration();  // HACK to enforce NaN check
        }
    }

    // MAIN ENTRY POINT for evaluation followed by gradient computation (forward prop then back prop)
    // TODO: pass a set of nodes instead of only one?
    // TODO: remove Evaluate() from here, instead call it at call site, and in here merely check whether everything is computed already
    template<class ElemType>
    void ComputationNetwork::ComputeGradient(const ComputationNodeBasePtr rootNode, 
                                             bool bResetToOne,                              // true if reset the gradient of rootnode to 1.0
                                             const Matrix<ElemType>* rootGradientInitValue, // if given then this is the starting gradient from the top
                                             bool bClearGradient,                           // if false then gradients are not cleared  --TODO: When does that happen?
                                             bool resetTimeStampAfterComputation)
    {
        // run forward pass first for criterion node
        // The actual call pattern is
        //  - Evaluate() for eval nodes
        //  - ComputeGradient() for the training criterion
        // I.e. we must call Evaluate() inside here as well, but it will typically only evaluate the training criterion bits because the eval nodes already require most of the network to be computed.
        Evaluate(rootNode);

        // TODO: comment what the purpose/condition of this is
        if (bClearGradient)
            ClearGradientForAllNodes(rootNode);

        // run backprop pass
        std::list<ComputationNodeBasePtr>& allNodes = GetGradientCalcOrder(rootNode);

        // TODO: do a runtime check for float vs. double. Also use the Is/AsPtr macros
        // The normal case is with the top root with a scalar gradient value of 1.0. This assumes a single and closure network. 
        // Allowing to not initialize to 1 allows network to be open to accept gradients from somewhere.
        // TODO: aren't these two mechanisms mutually exclusive?
        if (bResetToOne)
        {
            dynamic_pointer_cast<ComputationNode<ElemType>>(rootNode)->GradientValues().Resize(1, 1);   // TODO: make this a function of ComputationNode; but first need to get rid of Matrix<ElemType> here, or make it a local template parameter
            dynamic_pointer_cast<ComputationNode<ElemType>>(rootNode)->GradientValues().SetValue(1);
        }

        if (rootGradientInitValue != nullptr)   // user-specified gradient to start with
            dynamic_pointer_cast<ComputationNode<ElemType>>(rootNode)->GradientValues().SetValue(*rootGradientInitValue);

        // process nodes in pre-determined order
        for (auto & node : allNodes)
        {
#ifdef DISPLAY_DEBUG
            fprintf(stderr, "Compute Gradient For Node: %ls(%ls) Against Children\n", node->OperationName().c_str(), node->NodeName().c_str());
#endif
            // --- first, perform recurrent loops if this node participates in one

            RecurrentInfo * recInfo = FindInRecurrentLoops(node);
            if (recInfo)
            {
                if (!recInfo->m_completedGradient)
                {
                    const auto & recurrentNodes = recInfo->m_recurrentNodesForForward;
                    for (auto & node2 : recurrentNodes)
                        node2->OnComputeGradientBeginIteration();
                    auto pMBLayout = recurrentNodes[0]->GetMBLayout();
                    FrameRangeIteration range(pMBLayout, recInfo->m_steppingDirection);
                    for (auto t = range.rbegin(); t != range.rend(); t++)   // note: reverse iteration
                    {
                        for (auto nodeIter2 = recurrentNodes.rbegin(); nodeIter2 != recurrentNodes.rend(); ++nodeIter2)
                        {
                            auto & node2 = *nodeIter2;
                            node2->VerifyNumParallelSequences(GetNumParallelSequences());
                            if (IsNodeReqMultiSeqHandling(node2))
                                node2->MaskMissingGradientColumnsToZero(t);
                            node2->ComputeGradientForChildren(t);
                        }
                    }
                    for (auto & node2 : recurrentNodes)
                        node2->OnComputeGradientEndIteration();
                    recInfo->m_completedGradient = true;
                }
            }

            // --- second, do whole-batch operation if not recurrent

            else
            {
                node->OnComputeGradientBeginIteration();
                if (IsNodeReqMultiSeqHandling(node))    // (TODO: This will go away.)
                {
                    // batch is done only for feed-forward nodes
                    if (node->IsPartOfLoop()) // (this test was moved out from MaskMissingGradientColumnsToZero(void), it is likely unnecessary)
                        LogicError("Evaluate: Applying whole-MB operation to node that participates in a loop. This is likely wrong.");
                    node->MaskMissingGradientColumnsToZero(FrameRange(node->GetMBLayout()));
                }
                node->ComputeGradientForChildren(FrameRange(node->GetMBLayout()));
                node->OnComputeGradientEndIteration();
            }
        }

        //since we now allow sharing of the matrix for function value and gradient value. the function values are now destroyed
        //after gradient computation and need to be recomputed. This is indicated by the timestamp updated using this function
        //resetTimeStampAfterComputation is by default false because ComputeGradient in normal case is followed by new batch of input
        if (resetTimeStampAfterComputation)
            ResetEvalTimeStamp();
    }

    // find if node is part of a recurrent loop; and return the loop id
    // If found then return a pointer to the list of nodes of this loop.
    // TODO: This should just return &m_recurrentInfo of the matching loop, or nullptr if no match. If needed, m_recurrentInfo knows its loop id.
    ComputationNetwork::RecurrentInfo * ComputationNetwork::FindInRecurrentLoops(const ComputationNodeBasePtr& node)
    {
        // look in all recurrent loops of the network
        for (auto & iter : m_recurrentInfo)
            if (std::find(iter.m_recurrentNodes.begin(), iter.m_recurrentNodes.end(), node) != iter.m_recurrentNodes.end())
                return &iter;
        return nullptr;  // not part of a recurrent loop
    }

    bool ComputationNetwork::IsFuncValueOlderThanInputs(const vector<ComputationNodeBasePtr>& recurrentNodes)
    {
        for (auto ptr = recurrentNodes.begin(); ptr != recurrentNodes.end(); ptr++)
        {
            if ((*ptr)->IsFuncValueOlderThanInputs() && 
                (*ptr)->OperationName() != OperationNameOf(PastValueNode) &&
                (*ptr)->OperationName() != OperationNameOf(FutureValueNode))
            {
                return true;
            }
        }
        return false;
    }

    // for debugging
    void ComputationNetwork::PrintComputationTree(const ComputationNodeBasePtr& rootNode,
                                                  const bool forwardCompute,
                                                  const bool printMatrices)
    {
        std::list<ComputationNodeBasePtr> nodes;
        if (forwardCompute)
        {
            fprintf(stderr, "\n\nPrinting Forward Computation Node Order ... \n");
            nodes = GetEvalOrder(rootNode, false);
        }
        else
        {
            fprintf(stderr, "\n\nPrinting Gradient Computation Node Order ... \n");
            nodes = GetGradientCalcOrder(rootNode);
        }

        if (nodes.size() == 0)
        {
            fprintf(stderr, "\n$$$$ EMPTY !!!!!\n");
            return;
        }

        for (auto nodeIter = nodes.begin(); nodeIter != nodes.end(); nodeIter++)
        {
            ComputationNodeBasePtr node = (*nodeIter);
            node->PrintSelf(printMatrices);
        }
    }

    // -----------------------------------------------------------------------
    // validation
    // -----------------------------------------------------------------------

    // ValidateNetwork() - Validate the entire network
    // This calls ValidateNetowrk(Node) for all output nodes.
    // This is used after loading or for dumping the network.
    void ComputationNetwork::ValidateNetwork(bool allowFragment, const bool bAllowNoCriterion)
    {
        // currently only validates nodes, we should validate everything we can
        if (FeatureNodes().size() == 0 && !allowFragment)
            RuntimeError("No Feature nodes specified");

        // TODO: allocation does not belong here. This is called e.g. after loading. Memory should be allocated only when actually evaluating.
        AllocateAllEvalMatrices(EvaluationNodes(), OutputNodes(), FinalCriterionNodes());
        // first give criteria nodes as root node
        if (FinalCriterionNodes().size() > 0)
        {
            for (ComputationNodeBasePtr & node : FinalCriterionNodes())
            {
                if (!allowFragment)
                    FormRecurrentLoops(node);
#ifdef _DEBUG
                PrintComputationTree(node, false);
#endif
                //SetActualMiniBatchSizeFromFeatures();
                ValidateSubNetwork(node);
            }
        }
        else if (bAllowNoCriterion == true)
        {
            // do nothing
        }
        else if (!allowFragment)
            RuntimeError("No Criterion nodes specified");

        // now output nodes
        if (OutputNodes().size() > 0)
        {
            for (ComputationNodeBasePtr node : OutputNodes())
            {
                if (!allowFragment)
                    FormRecurrentLoops(node);
                ValidateSubNetwork(node);
            }
        }
        else if (!allowFragment)
            RuntimeError("No Output nodes specified");

        // now evaluation nodes
        if (EvaluationNodes().size() > 0)
        {
            for (ComputationNodeBasePtr node : EvaluationNodes())
            {
                if (!allowFragment)
                    FormRecurrentLoops(node);
                ValidateSubNetwork(node);
            }
        }
    }

    // validate sub-network needed to evalute a specific output node
    // This calls Validate() on every node in evaluation order (allowing to propagate things forwards through the net).
    // This is called lazily but once only per node until next ClearCache().
    // This also sets up MBLayout links.
    // TODO: I can't see a clear pattern when ClearCache() is called. E.g. at the start of each epoch? Or never in normal operation (init only at construction)?
    // Note: under some circumstances, one must call FormRecurrentNodes() on this node before calling this. TODO: Not clear which ones.
    // TODO: ^^ is this really needed? Can we just call it inside?
    void ComputationNetwork::ValidateSubNetwork(const ComputationNodeBasePtr& rootNode)
    {
        // set up MBLayout links of inputs (all others get propagated upwards through Validate())
        // TODO: Once we support mismatching layouts, this will be more involved. For now, everything shares the one layout that the Network knows about.
        for (auto node : InputNodes(rootNode))
        {
            node->LinkToMBLayout(m_pMBLayout);
            // handle the special case of being validated before reading a minibatch
            // In that case, the layout is empty. We set up a dummy layout to match the first InputValue.
            // TODO: This is a stop-gap. We need a better-controlled way of when what gets validated.
            if (m_pMBLayout->GetNumCols() == 0)
                m_pMBLayout->Init(1, node->GetNumCols(), false);
        }

        // we call all nodes' Validate() in order to validate, that is, set up MBLayout and FunctionValues dimension
        // A problem is that recurrent loops may require partial validation.
        // Nodes validated on partial input (i.e. some children not yet validated) will be revisited.
        const auto & nodes = GetEvalOrder(rootNode, false);

        for (auto & node : nodes)
        {
            node->m_visited = false;
            node->m_needsGradient = node->IsParameterUpdateRequired();  // these get propagated upwards in the following
        }

        // loop and validate until we are done
        // steps:
        //  - validate (not final)          // not final means no dimension checks
        //    Keep going through the list until all nodes have been validated and all inputs have been validated as well.
        //  - validate (final)              // final means consistency checks
        //    Fail if any change during this stage.
        size_t pass = 0;
        size_t toValidate = nodes.size();
        while (toValidate > 0)
        {
            pass++;
            fprintf(stderr, "\n\nValidating for node %ls. %d nodes to process in pass %d.\n", rootNode->NodeName().c_str(), (int)toValidate, (int)pass);
            ValidateNodes(nodes, false/*isFinalValidationPass*/, toValidate);
        }
        fprintf(stderr, "\n\nValidating for node %ls, final verification.\n", rootNode->NodeName().c_str());
        ValidateNodes(nodes, true/*isFinalValidationPass*/, toValidate);
        if (toValidate != 0)
            LogicError("ValidateSubNetwork: ValidateNodes(true) unexpectedly returned with work left to do.");

        for (auto & node : nodes)
        {
#if 0       // not possible once we have inconsistent layouts
            // verify that the contract with MB layout was obeyed by Validate()
            if (node->GetMBLayout() && node->GetMBLayout()->GetNumCols() != node->GetNumCols())
            {
                fprintf(stderr, "\n%ls %ls operation's Validate() function set function values width (%d) inconsistent with MB layout width (T=%d x S=%d)\n",
                        node->NodeName().c_str(), node->OperationName().c_str(), (int)node->GetNumCols(), (int)node->GetNumTimeSteps(), (int)node->GetNumParallelSequences());
                LogicError("%ls %ls operation's Validate() function set function values width (%d) inconsistent with MB layout width (T=%d x S=%d)",
                           node->NodeName().c_str(), node->OperationName().c_str(), (int)node->GetNumCols(), (int)node->GetNumTimeSteps(), (int)node->GetNumParallelSequences());
            }
#endif
            // nodes must output non-zero dimensional data, otherwise assume user error
            if (node->GetNumRows() == 0 && (node->GetMBLayout() || node->GetNumCols() == 0))
                RuntimeError("%ls operation has 0 elements", node->NodeName().c_str());
        }
        fprintf(stderr, "\n\n");

        // logging the non-default-layout nodes
        vector<ComputationNodeBasePtr> nonDefaultNodes;
        for (auto node : nodes)
        {
            if (!(node->GetMBLayout() == m_pMBLayout))
                nonDefaultNodes.push_back(node);
        }
        if (!nonDefaultNodes.empty())
        {
            fprintf(stderr, "%d out of %d nodes do not share the minibatch layout with the input data.\n\n", (int)nonDefaultNodes.size(), (int)nodes.size());
            //for (auto node : nonDefaultNodes)
            //    fprintf(stderr, "    %ls\n", node->NodeName().c_str());
            //fprintf(stderr, "\n\n");
        }
    }

    void ComputationNetwork::ValidateNodes(list<ComputationNodeBasePtr> nodes, bool isFinalValidationPass, size_t & todo)
    {
        todo = 0;           // returns how many nodes are to be redone
        for (auto & node : nodes)
        {
            const auto & children = node->GetChildren();
            const bool isLeaf = node->IsLeaf();
            // only validate a node if it has at least one child
            bool hasVisitedChild = false;
            bool allChildrenVisited = true;
            for (auto & child : children)
            {
                hasVisitedChild |= child->m_visited;    // if not a single visited child then no point in validating
                allChildrenVisited &= child->m_visited;
            }
            // if there is not at least one visited child
            bool valid = false;
            if (hasVisitedChild || isLeaf)
            {
                // got at least one child: it makes sense to call Validate()
                // keep state
                MBLayoutPtr oldMBLayoutPtr = node->GetMBLayout();
                auto dim = node->GetDims();
                vector<pair<size_t, size_t>> childDims;
                for (auto & child : children)
                    childDims.push_back(child->GetDims());
                auto imageLayouts = node->GetImageLayouts();
                // We do call validate(final) as many times as needed, since stuff may have changed underneath.
                node->PrintSelfBeforeValidation();
                node->Validate(isFinalValidationPass/*final*/);      // all nodes have been visited: do verification instead of just inference
                fprintf(stderr, " -> [%lu, %s%lu]", node->GetNumRows(), node->HasMBLayout() ? "MBSize " : "", node->GetNumCols());
                node->m_visited = true;
                // also take the opportunity to propagate m_needsGradient
                auto needsGradient = node->m_needsGradient;
                for (auto & child : children)       // TODO: do we need a check that this is stable if isFinalValidationPass?
                    node->m_needsGradient |= child->m_needsGradient;
                // check state --node will be valid if all nodes have been visited and node has not been updated
                bool unchanged = true;
                unchanged &= (oldMBLayoutPtr == node->GetMBLayout());
                unchanged &= (dim == node->GetDims());
                vector<pair<size_t, size_t>> newChildDims;
                for (auto & child : children)
                    newChildDims.push_back(child->GetDims());
                unchanged &= (childDims == newChildDims);
                unchanged &= (imageLayouts == node->GetImageLayouts());
                unchanged &= (needsGradient == node->m_needsGradient);
                if (isFinalValidationPass && !unchanged)
                    LogicError("ValidateSubNetwork: %ls %ls operation changed during final validation.", node->NodeName().c_str(), node->OperationName().c_str());
                if (isFinalValidationPass && !allChildrenVisited)
                    LogicError("ValidateSubNetwork: %ls %ls operation in final validation although not all children were visited?", node->NodeName().c_str(), node->OperationName().c_str());
                // if all children valid then 
                valid = (allChildrenVisited && unchanged) || isLeaf;
            }
            // count those that we need to redo
            if (!valid)
                todo++;
        }
    }

    // prepare to compute with the subnetwork that this rootNode depends on, including
    //  - auto-detecting recurrent loops
    //  - collect input and learnable nodes
    //  - calling Validate() on all nodes lazily, which sizes all matrices (column dimensions get updated to MB size)
    // Done lazily, called for every minibatch's invocation of EvaluateNode(), but memoizing which nodes were done already.
    // BUGBUG? Lazy triggers on the root node. I.e. for two different root nodes (training, eval), it validates twice.
    void ComputationNetwork::BuildAndValidateSubNetwork(const ComputationNodeBasePtr rootNode)
    {
        const auto inserted = m_built.insert(rootNode).second;  // remember we built it
        if (!inserted)
            return;                                             // already done

        // detect recurrent loops for this root node
        // TODO: not nice--why not always call this in ValidateSubNetwork() only?
        FormRecurrentLoops(rootNode);

        // for the m_inputs and m_learnableParameters sets for this rootNode
        CollectInputAndLearnableParameters(rootNode);

        // validate the rootNode and all nodes it depends on, in evaluation order
        ValidateSubNetwork(rootNode);

        // (gone: now done more directly without state in ComputationNode)
        //SetRequestNodesMultiSeqHandling();
    }

    bool ComputationNetwork::BuiltAndValidatedSubNetwork(const ComputationNodeBasePtr & rootNode)
    {
        return m_built.find(rootNode) != m_built.end();
    }

}}}
