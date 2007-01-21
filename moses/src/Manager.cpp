// $Id$
// vim:tabstop=2

/***********************************************************************
Moses - factored phrase-based language decoder
Copyright (C) 2006 University of Edinburgh

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
***********************************************************************/
#include <limits>
#include <cmath>
#include "Manager.h"
#include "TypeDef.h"
#include "Util.h"
#include "TargetPhrase.h"
#include "LatticePath.h"
#include "LatticePathCollection.h"
#include "TranslationOption.h"
#include "LMList.h"
#include "TranslationOptionCollection.h"

using namespace std;

Manager::Manager(InputType const& source, StaticData &staticData)
:m_source(source)
,m_hypoStack(source.GetSize(), staticData.GetDecodeStepList())
,m_staticData(staticData)
,m_transOptColl(source.CreateTranslationOptionCollection())
,m_initialTargetPhrase(Output)
{
	HypothesisStack::iterator iterStack;
	for (iterStack = m_hypoStack.begin() ; iterStack != m_hypoStack.end() ; ++iterStack)
	{
		HypothesisCollection &sourceHypoColl = *iterStack;
		sourceHypoColl.SetMaxHypoStackSize(m_staticData.GetMaxHypoStackSize());
		sourceHypoColl.SetBeamThreshold(m_staticData.GetBeamThreshold());
	}
}

Manager::~Manager() 
{
  delete m_transOptColl;
}

/**
 * Main decoder loop that translates a sentence by expanding
 * hypotheses stack by stack, until the end of the sentence.
 */
void Manager::ProcessSentence()
{	
	m_staticData.ResetSentenceStats(m_source);
	const vector<DecodeStep*> &decodeStepList = m_staticData.GetDecodeStepList();
	// create list of all possible translations
	// this is only valid if:
	//		1. generation of source sentence is not done 1st
	//		2. initial hypothesis factors are given in the sentence
	//CreateTranslationOptions(m_source, phraseDictionary, lmListInitial);
	m_transOptColl->CreateTranslationOptions(decodeStepList
  														, m_staticData.GetFactorCollection());

	// initial seed hypothesis: nothing translated, no words produced
	{
		Hypothesis *hypo = Hypothesis::Create(m_source, decodeStepList, m_initialTargetPhrase);
		m_hypoStack.GetStack(0).AddPrune(hypo);
	}
	
	// go through each stack
	HypothesisStack::iterator iterStack;
	for (iterStack = m_hypoStack.begin() ; iterStack != m_hypoStack.end() ; ++iterStack)
	{
		HypothesisCollection &sourceHypoColl = *iterStack;

		// the stack is pruned before processing (lazy pruning):
		VERBOSE(3,"processing hypothesis from next stack");
		sourceHypoColl.PruneToSize(m_staticData.GetMaxHypoStackSize());
		VERBOSE(3,std::endl);
		sourceHypoColl.CleanupArcList();

		// go through each hypothesis on the stack and try to expand it
		HypothesisCollection::const_iterator iterHypo;
		for (iterHypo = sourceHypoColl.begin() ; iterHypo != sourceHypoColl.end() ; ++iterHypo)
		{
			Hypothesis &hypothesis = **iterHypo;
			ProcessOneHypothesis(hypothesis, decodeStepList); // expand the hypothesis
		}
		// some logging
		IFVERBOSE(2) { OutputHypoStackSize(); }
		//OutputHypoStackSize();
	}

	//OutputHypoStack();
	OutputHypoStackSize();

	// some more logging
	VERBOSE(2,m_staticData.GetSentenceStats());
}


/** Find all translation options to expand one hypothesis, trigger expansion
 * this is mostly a check for overlap with already covered words, and for
 * violation of reordering limits. 
 * \param hypothesis hypothesis to be expanded upon
 */
void Manager::ProcessOneHypothesis(const Hypothesis &hypothesis, const std::vector<DecodeStep*> &decodeStepList)
{
	// since we check for reordering limits, its good to have that limit handy
	int maxDistortion = m_staticData.GetMaxDistortion();

	// no limit of reordering: only check for overlap
	if (maxDistortion < 0)
	{	
		const WordsBitmap &hypoBitmap	= hypothesis.GetSourceBitmap();

		std::vector<DecodeStep*>::const_iterator iter;
		for (iter = decodeStepList.begin() ; iter != decodeStepList.end() ; ++iter)
		{
			const DecodeStep &decodeStep	= **iter;
			size_t decodeStepId						= decodeStep.GetId();
			const size_t hypoFirstGapPos	= hypoBitmap.GetFirstGapPos(decodeStepId)
									, sourceSize			= m_source.GetSize();

			for (size_t startPos = hypoFirstGapPos ; startPos < sourceSize ; ++startPos)
			{
				for (size_t endPos = startPos ; endPos < sourceSize ; ++endPos)
				{
					if (!hypoBitmap.IsHierarchy(decodeStepId, startPos, endPos))
						break;

					if (!hypoBitmap.Overlap(WordsRange(decodeStepId, startPos, endPos)))
					{
						ExpandAllHypotheses(hypothesis
													, m_transOptColl->GetTranslationOptionList(
																																WordsRange(decodeStepId, startPos, endPos)));
					}
				}

			}
		}
		return; // done with special case (no reordering limit)
	}

	// if there are reordering limits, make sure it is not violated
	// the coverage bitmap is handy here (and the position of the first gap)
	const WordsBitmap &hypoBitmap = hypothesis.GetSourceBitmap();

	std::vector<DecodeStep*>::const_iterator iter;
	for (iter = decodeStepList.begin() ; iter != decodeStepList.end() ; ++iter)
	{
		const DecodeStep &decodeStep	= **iter;
		size_t decodeStepId						= decodeStep.GetId();
		const size_t hypoWordCount		= hypoBitmap.GetNumWordsCovered(decodeStepId)
								, hypoFirstGapPos	= hypoBitmap.GetFirstGapPos(decodeStepId)
								, sourceSize			= m_source.GetSize();
		
		// MAIN LOOP. go through each possible hypo
		for (size_t startPos = hypoFirstGapPos ; startPos < sourceSize ; ++startPos)
		{
			for (size_t endPos = startPos ; endPos < sourceSize ; ++endPos)
			{
				if (!hypoBitmap.IsHierarchy(decodeStepId, startPos, endPos))
					break;

				// no gap so far => don't skip more than allowed limit
				if (hypoFirstGapPos == hypoWordCount)
				{
					if (startPos == hypoWordCount
							|| (startPos > hypoWordCount 
									&& endPos <= hypoWordCount + maxDistortion)
						)
					{
						ExpandAllHypotheses(hypothesis
													,m_transOptColl->GetTranslationOptionList(WordsRange(decodeStepId, startPos, endPos)));
					}
				}
				// filling in gap => just check for overlap
				else if (startPos < hypoWordCount)
				{
					if (startPos >= hypoFirstGapPos
						&& !hypoBitmap.Overlap(WordsRange(decodeStepId, startPos, endPos)))
					{
						ExpandAllHypotheses(hypothesis
													,m_transOptColl->GetTranslationOptionList(WordsRange(decodeStep.GetId(), startPos, endPos)));
					}
				}
				// ignoring, continuing forward => be limited by start of gap
				else
				{
					if (endPos <= hypoFirstGapPos + maxDistortion
							&& !hypoBitmap.Overlap(WordsRange(decodeStepId, startPos, endPos)))
					{
						ExpandAllHypotheses(hypothesis
														,m_transOptColl->GetTranslationOptionList(WordsRange(decodeStepId, startPos, endPos)));
					}
				}
			}
		}
	}
}

/**
 * Expand a hypothesis given a list of translation options
 * \param hypothesis hypothesis to be expanded upon
 * \param transOptList list of translation options to be applied
 */

void Manager::ExpandAllHypotheses(const Hypothesis &hypothesis,const TranslationOptionList &transOptList)
{
	TranslationOptionList::const_iterator iter;
	for (iter = transOptList.begin() ; iter != transOptList.end() ; ++iter)
	{
		ExpandHypothesis(hypothesis, **iter);
	}
}


/**
 * Expand one hypothesis with a translation option.
 * this involves initial creation, scoring and adding it to the proper stack
 * \param hypothesis hypothesis to be expanded upon
 * \param transOpt translation option (phrase translation) 
 *        that is applied to create the new hypothesis
 */

void Manager::ExpandHypothesis(const Hypothesis &hypothesis, const TranslationOption &transOpt) 
{
	const AlignmentPhrase 
							&hypoAlignment = hypothesis.GetTargetAlignment()
						, &targetAlignment = transOpt.GetAlignmentPair().GetPhraseAlignVec(Output);
	size_t nextStartPos = hypothesis.GetNextStartPos(transOpt);
	if (hypoAlignment.IsCompatible(targetAlignment, nextStartPos))
	{
		// create hypothesis and calculate all its scores
		Hypothesis *newHypo = hypothesis.CreateNext(transOpt);
		newHypo->CalcScore(m_staticData, m_transOptColl->GetFutureScoreObject());
		// logging for the curious
		IFVERBOSE(3) {
			newHypo->PrintHypothesis(m_source, m_staticData.GetWeightDistortion(), m_staticData.GetWeightWordPenalty());
		}

		// add to hypothesis stack
		m_hypoStack.AddPrune(newHypo);
	}
}

/**
 * Find best hypothesis on the last stack.
 * This is the end point of the best translation, which can be traced back from here
 */
const Hypothesis *Manager::GetBestHypothesis() const
{
	const HypothesisCollection &hypoColl = m_hypoStack.back();
	return hypoColl.GetBestHypothesis();
}

/**
 * Logging of hypothesis stack sizes
 */
void Manager::OutputHypoStackSize()
{
	HypothesisStack::iterator iterStack = m_hypoStack.begin();
	TRACE_ERR( "Stack sizes: " << (*iterStack).size());
	for (++iterStack; iterStack != m_hypoStack.end() ; ++iterStack)
	{
		TRACE_ERR( ", " << (*iterStack).size());
	}
	TRACE_ERR( endl);
}

/**
 * Logging of hypothesis stack contents
 * \param stack number of stack to be reported, report all stacks if 0 
 */
void Manager::OutputHypoStack(int stack)
{
	if (stack >= 0)
	{
		TRACE_ERR( "Stack " << stack << ": " << endl << m_hypoStack.GetStack(stack) << endl);
	}
	else
	{ // all stacks
		int i = 0;
		HypothesisStack::iterator iterStack;
		for (iterStack = m_hypoStack.begin() ; iterStack != m_hypoStack.end() ; ++iterStack)
		{
			HypothesisCollection &hypoColl = *iterStack;
			TRACE_ERR( "Stack " << i++ << ": " << endl << hypoColl << endl);
		}
	}
}
void GetSurfacePhrase(std::vector<size_t>& tphrase, LatticePath const& path)
{
	tphrase.clear();
	const Phrase &targetPhrase = path.GetTargetPhrase();

	for (size_t pos = 0 ; pos < targetPhrase.GetSize() ; ++pos)
	{
		const Factor *factor = targetPhrase.GetFactor(pos, 0);
		assert(factor);
		tphrase.push_back(factor->GetId());
	}
}

/**
 * After decoding, the hypotheses in the stacks and additional arcs
 * form a search graph that can be mined for n-best lists.
 * The heavy lifting is done in the LatticePath and LatticePathCollection
 * this function controls this for one sentence.
 *
 * \param count the number of n-best translations to produce
 * \param ret holds the n-best list that was calculated
 */
void Manager::CalcNBest(size_t count, LatticePathList &ret,bool onlyDistinct) const
{
	if (count <= 0)
		return;

	vector<const Hypothesis*> sortedPureHypo = m_hypoStack.back().GetSortedList();

	if (sortedPureHypo.size() == 0)
		return;

	LatticePathCollection contenders;

	set<std::vector<size_t> > distinctHyps;

	// add all pure paths
	vector<const Hypothesis*>::const_iterator iterBestHypo;
	for (iterBestHypo = sortedPureHypo.begin() 
			; iterBestHypo != sortedPureHypo.end()
			; ++iterBestHypo)
	{
		contenders.Add(new LatticePath(*iterBestHypo));
	}

	// MAIN loop
	for (size_t iteration = 0 ; (onlyDistinct ? distinctHyps.size() : ret.GetSize()) < count && contenders.GetSize() > 0 && (iteration < count * 20) ; iteration++)
	{
		// get next best from list of contenders
		LatticePath *path = contenders.pop();
		assert(path);
		bool addPath = true;
		if(onlyDistinct)
		{
			// TODO - not entirely correct.
			// output phrase can't be assumed to only contain factor 0.
			// have to look in StaticData.GetOutputFactorOrder() to find out what output factors should be
			std::vector<size_t> tgtPhrase;
			GetSurfacePhrase(tgtPhrase,*path);
			addPath = distinctHyps.insert(tgtPhrase).second;
		}
		
		if(addPath && ret.Add(path)) 
		{	// create deviations from current best
			path->CreateDeviantPaths(contenders);		
		}
		else
			delete path;

		if(!onlyDistinct)
		{
			contenders.Prune(count);
		}
	}
}

void Manager::CalcDecoderStatistics(const StaticData& staticData) const 
{
  const Hypothesis *hypo = GetBestHypothesis();
	if (hypo != NULL)
  {
  	staticData.GetSentenceStats().CalcFinalStats(*hypo);
    IFVERBOSE(2) {
		 	if (hypo != NULL) {
		   	string buff;
		  	string buff2;
		   	TRACE_ERR( "Source and Target Units:"
		 							<< hypo->GetSourcePhrase());
				buff2.insert(0,"] ");
				buff2.insert(0,(hypo->GetCurrTargetPhrase()).ToString());
				buff2.insert(0,":");
				buff2.insert(0,(hypo->GetCurrSourceWordsRange()).ToString());
				buff2.insert(0,"[");
				
				hypo = hypo->GetPrevHypo();
				while (hypo != NULL) {
					//dont print out the empty final hypo
				  buff.insert(0,buff2);
				  buff2.clear();
				  buff2.insert(0,"] ");
				  buff2.insert(0,(hypo->GetCurrTargetPhrase()).ToString());
				  buff2.insert(0,":");
				  buff2.insert(0,(hypo->GetCurrSourceWordsRange()).ToString());
				  buff2.insert(0,"[");
				  hypo = hypo->GetPrevHypo();
				}
				TRACE_ERR( buff << endl);
      }
    }
  }
}
