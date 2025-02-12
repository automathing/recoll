/* Copyright (C) 2004-2022 J.F.Dockes
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "autoconfig.h"

#include <unordered_map>
#include <deque>
#include <algorithm>
#include <regex>

//#define LOGGER_LOCAL_LOGINC 3
#include "log.h"
#include "rcldb.h"
#include "rcldb_p.h"
#include "rclquery.h"
#include "rclquery_p.h"
#include "textsplit.h"
#include "hldata.h"
#include "chrono.h"
#include "unacpp.h"
#include "zlibut.h"
#include "rcldoc.h"
#include "searchdata.h"

using namespace std;

#undef DEBUGABSTRACT  
#ifdef DEBUGABSTRACT
#define LOGABS LOGINF
#else
#define LOGABS LOGDEB1
#endif

// We now let plaintorich do the highlight tags insertions which is
// wasteful because we have most of the information (but the perf hit
// is small because it's only called on the output fragments, not on
// the whole text). The highlight zone computation code has been left
// around just in case I change my mind.
#undef COMPUTE_HLZONES

namespace Rcl {

#ifndef NO_STD_REGEX
//// Fragment cleanup
// Chars we turn to spaces in the Snippets
static const string cstr_nc("\n\r\x0c\\");
// Things that we don't want to repeat in a displayed snippet.
// e.g.  > > > > > >
static const string punctcls("[-<>._+,#*=|]");
static const string punctRE = "(" + punctcls +  " *)(" + punctcls + " *)+";
static std::regex fixfrag_re(punctRE);
static const string punctRep{"$2"};
static string fixfrag(const string& infrag)
{
    return std::regex_replace(neutchars(infrag, cstr_nc), fixfrag_re, punctRep);
}
#else
static string fixfrag(const string& infrag)
{
    return infrag;
}
#endif


// Fragment descriptor. A fragment is a text area with one or several
// matched terms and some context. It is ranked according to the
// matched term weights and the near/phrase matches get a boost.
struct MatchFragment {
    // Start/End byte offsets of fragment in the document text
    size_t start;
    size_t stop;
    // Weight for this fragment (bigger better)
    double coef;
#ifdef COMPUTE_HLZONES
    // Highlight areas (each is one or several contiguous match
    // terms). Because a fragment extends around a match, there
    // can be several contiguous or separate matches in a given
    // fragment.
    vector<pair<size_t,size_t>> hlzones;
#endif
    // Position of the first matched term (for page number computations)
    size_t hitpos;
    // "best term" for this match (e.g. for use as ext app search term)
    string term;
    int line;
    
    MatchFragment(size_t sta, size_t sto, double c,
#ifdef COMPUTE_HLZONES
                  vector<pair<size_t,size_t>>& hl,
#endif
                  size_t pos, string& trm, int ln) 
        : start(sta), stop(sto), coef(c), hitpos(pos), line(ln) {
#ifdef COMPUTE_HLZONES
        hlzones.swap(hl);
#endif
        term.swap(trm);
    }
};


// Text splitter for finding the match areas in the document text.
class TextSplitABS : public TextSplit {
public:

    TextSplitABS(const string& rawtext, const vector<string>& matchTerms,
                 const HighlightData& hdata,
                 unordered_map<string, double>& wordcoefs,
                 unsigned int ctxwords,
                 Flags flags,
                 unsigned int maxterms)
        :  TextSplit(flags), m_rawtext(rawtext),
           m_terms(matchTerms.begin(), matchTerms.end()),
           m_hdata(hdata), m_wordcoefs(wordcoefs), m_ctxwords(ctxwords),
           maxtermcount(maxterms) {

        // Take note of the group (phrase/near) terms because we need
        // to compute the position lists for them.
        for (const auto& tg : hdata.index_term_groups) {
            if (tg.kind != HighlightData::TermGroup::TGK_TERM) {
                for (const auto& group : tg.orgroups) {
                    for (const auto& term: group) {
                        m_gterms.insert(term);
                    }
                }
            }
        }
    }

    virtual void newline(size_t) override {
        m_line++;
    }

    // Accept a word and its position. If the word is a matched term,
    // add/update fragment definition.
    virtual bool takeword(const std::string& term, size_t pos, size_t bts, size_t bte) override {
        //LOGABS("abs:takeword: [" << term << "] pos " << pos << " bpos: "<< bts << ":" << bte << "\n");
        // Limit time taken with monster documents. The resulting abstract will be incorrect or
        // inexistent, but this is better than taking forever (the default cutoff value comes from
        // the snippetMaxPosWalk configuration parameter, and is 10E6)
        if (maxtermcount && termcount++ > maxtermcount) {
            LOGINF("Rclabsfromtext: stopping because maxtermcount reached: "<<maxtermcount << "\n");
            retflags |= ABSRES_TRUNC;
            return false;
        }
        // Also limit the number of fragments (just in case safety)
        if (m_fragments.size() > maxtermcount / 100) {
            LOGINF("Rclabsfromtext: stopping: max fragments count: " << maxtermcount/100 << "\n");
            retflags |= ABSRES_TRUNC;
            return false;
        }
        // Remember recent past
        m_prevterms.push_back({bts, bte});
        if (m_prevterms.size() > m_ctxwords+1) {
            m_prevterms.pop_front();
        }

        string dumb;
        if (o_index_stripchars) {
            if (!unacmaybefold(term, dumb, UNACOP_UNACFOLD)) {
                LOGINFO("abstract: unac failed for [" << term << "]\n");
                return true;
            }
        } else {
            dumb = term;
        }

        if (m_terms.find(dumb) != m_terms.end()) {
            PRETEND_USE(m_rawtext);
            // This word is a search term. Extend or create fragment
            LOGABS("match: [" << dumb << "] pos " << pos << " bpos " << bts <<
                    ":" << bte << " remainingWords " << m_remainingWords << "\n");
            LOGABS("Match text " << m_rawtext.substr(bts, bte - bts) << "\n");
            double coef = m_wordcoefs[dumb];
            if (!m_remainingWords) {
                // No current fragment. Start one
                m_curhitpos = baseTextPosition + pos;
                m_curfrag.first = m_prevterms.front().first;
                m_curfrag.second = m_prevterms.back().second;
#ifdef COMPUTE_HLZONES
                m_curhlzones.push_back(pair<size_t,size_t>(bts, bte));
#endif
                m_curterm = term;
                m_curtermcoef = coef;
                m_curfragline = m_line;
            } else {
                LOGABS("Extending current fragment: "<<m_remainingWords<<" -> "<<m_ctxwords<< "\n");
                m_extcount++;
#ifdef COMPUTE_HLZONES
                if (m_prevwordhit) {
                    m_curhlzones.back().second = bte;
                } else {
                    m_curhlzones.push_back(pair<size_t,size_t>(bts, bte));
                }
#endif
                if (coef > m_curtermcoef) {
                    m_curterm = term;
                    m_curtermcoef = coef;
                }
            }

#ifdef COMPUTE_HLZONES
            m_prevwordhit = true;
#endif
            m_curfragcoef += coef;
            m_remainingWords = m_ctxwords + 1;
            if (m_extcount > 5) {
                // Limit expansion of contiguous fragments (this is to avoid common terms in search
                // causing long heavyweight meaningless fragments. Also, limit length).
                m_remainingWords = 1;
                m_extcount = 0;
            }

            // If the term is part of a near/phrase group, update its positions list
            if (m_gterms.find(dumb) != m_gterms.end()) {
                // Term group (phrase/near) handling
                m_plists[dumb].push_back(pos);
                m_gpostobytes[pos] = {bts, bte};
                LOGABS("Recorded bpos for pos " << pos << ": " << bts << " " << bte << "\n");
            }
        }
#ifdef COMPUTE_HLZONES
        else {
            // Not a matched term
            m_prevwordhit = false;
        }
#endif

        
        if (m_remainingWords) {
            // Fragment currently open. Time to close ?
            m_remainingWords--;
            m_curfrag.second = bte;
            if (m_remainingWords == 0) {
                LOGABS("FRAGMENT: from byte " << m_curfrag.first <<
                        " to  byte " << m_curfrag.second << "\n");
                LOGABS("FRAGMENT TEXT [" << m_rawtext.substr(
                            m_curfrag.first, m_curfrag.second-m_curfrag.first) << "]\n");
                // We used to not push weak fragments if we had a lot already. This can cause
                // problems if the fragments we drop are actually group fragments (which have not
                // got their boost yet). The right cut value is difficult to determine, because the
                // absolute values of the coefs depend on many things (index size, etc.) The old
                // test was if (m_totalcoef < 5.0 || m_curfragcoef >= 1.0) We now just avoid
                // creating a monster by testing the current fragments count at the top of the
                // function
                m_fragments.push_back(MatchFragment(m_curfrag.first, m_curfrag.second, m_curfragcoef,
#ifdef COMPUTE_HLZONES
                                                    m_curhlzones,
#endif
                                                    m_curhitpos, m_curterm, m_curfragline));
                m_totalcoef += m_curfragcoef;
                m_curfragcoef = 0.0;
                m_curtermcoef = 0.0;
            }
        }
        return true;
    }
    
    const vector<MatchFragment>& getFragments() {
        return m_fragments;
    }


    // After the text is split: use the group terms positions lists to
    // find the group matches.
    void updgroups() {
        // Possibly store current incomplete fragment (if match was
        // close to the end of the text, so we did not close it):
        if (m_curtermcoef != 0.0) {
            m_fragments.push_back(MatchFragment(m_curfrag.first, m_curfrag.second, m_curfragcoef,
#ifdef COMPUTE_HLZONES
                                                m_curhlzones,
#endif
                                                m_curhitpos, m_curterm, m_curfragline));
            m_totalcoef += m_curfragcoef;
            m_curfragcoef = 0.0;
            m_curtermcoef = 0.0;
        }

        LOGDEB("TextSplitABS: stored total " << m_fragments.size() << " fragments" << "\n");
        vector<GroupMatchEntry> tboffs;

        // Look for matches to PHRASE and NEAR term groups and finalize the matched regions list
        // (sort it by increasing start then decreasing length).
        for (unsigned int i = 0; i < m_hdata.index_term_groups.size(); i++) {
            if (m_hdata.index_term_groups[i].kind != HighlightData::TermGroup::TGK_TERM) {
                matchGroup(m_hdata, i, m_plists, m_gpostobytes, tboffs);
            }
        }

        // Sort the fragments by increasing start and decreasing width
        std::sort(m_fragments.begin(), m_fragments.end(),
                  [](const MatchFragment& a, const MatchFragment& b) -> bool {
                      if (a.start != b.start)
                          return a.start < b.start;
                      return a.stop - a.start > b.stop - a.stop;
                  }
            );
        
        // Sort the group regions by increasing start and decreasing width.  
        std::sort(tboffs.begin(), tboffs.end(),
                  [](const GroupMatchEntry& a, const GroupMatchEntry& b)
                  -> bool {
                      if (a.offs.first != b.offs.first)
                          return a.offs.first < b.offs.first;
                      return a.offs.second > b.offs.second;
                  }
            );

        // Give a boost to fragments which contain a group match (phrase/near), they are dear to the
        // user's heart. Lists are sorted, so we never go back in the fragment list (can always
        // start the search where we previously stopped).
        if (m_fragments.empty()) {
            return;
        }
        auto fragit = m_fragments.begin();
        for (const auto& grpmatch : tboffs) {
            LOGABS("LOOKING FOR FRAGMENT: group: " << grpmatch.offs.first << "-" <<
                    grpmatch.offs.second<<" curfrag "<<fragit->start<<"-"<<fragit->stop<<"\n");
            while (fragit->stop < grpmatch.offs.first) {
                fragit++;
                if (fragit == m_fragments.end()) {
                    return;
                }
            }
            if (fragit->start <= grpmatch.offs.first && fragit->stop >= grpmatch.offs.second) {
                // grp in frag
                fragit->coef += 10.0;
            }
        }

        return;
    }

    int getretflags() {
        return retflags;
    }
    
private:
    const string& m_rawtext;
    // Past terms because we need to go back for context before a hit
    deque<pair<size_t, size_t>>  m_prevterms;
    // Data about the fragment we are building
    pair<size_t, size_t> m_curfrag{0,0};
    int m_curfragline{0};
    double m_curfragcoef{0.0};
    unsigned int m_remainingWords{0};
    unsigned int m_extcount{0};
#ifdef COMPUTE_HLZONES
    vector<pair<int,int>> m_curhlzones;
    bool m_prevwordhit{false};
#endif
    // Current sum of fragment weights
    double m_totalcoef{0.0};
    // Position of 1st term match (for page number computations)
    size_t m_curhitpos{0};
    // "best" term
    string m_curterm;
    double m_curtermcoef{0.0};
    int m_line{1};

    // Group terms, extracted from m_hdata 
    unordered_set<string> m_gterms;
    // group/near terms word positions.
    unordered_map<string, vector<size_t>> m_plists;
    unordered_map<size_t, pair<size_t, size_t> > m_gpostobytes;
    
    // Input
    unordered_set<string> m_terms;
    const HighlightData& m_hdata;
    unordered_map<string, double>& m_wordcoefs;
    unsigned int m_ctxwords;

    // Result: begin and end byte positions of query terms/groups in text
    vector<MatchFragment> m_fragments;

    unsigned int termcount{0};
    unsigned int maxtermcount{0};
    int retflags{0};
};

int Query::Native::abstractFromText(
    Rcl::Db::Native *ndb,
    Xapian::docid docid,
    const vector<string>& matchTerms,
    const multimap<double, vector<string>> byQ,
    double,
    int ctxwords,
    unsigned int maxtotaloccs,
    vector<Snippet>& vabs,
    Chrono& chron,
    bool sortbypage
    )
{
    PRETEND_USE(chron);
    LOGABS("abstractFromText: entry: " << chron.millis() << "mS\n");

    std::string udi;
    if (!ndb->docidToUdi(docid, udi)) {
        LOGERR("Query::Native::abstractFromText: can't retrieve udi\n");
        return ABSRES_ERROR;
    }
        
    string rawtext;
    if (!ndb->getRawText(udi, docid, rawtext)) {
        LOGDEB0("abstractFromText: can't fetch text\n");
        return ABSRES_ERROR;
    }
    LOGABS("abstractFromText: got raw text: size "<<rawtext.size()<<" "<<chron.millis()<<"mS\n");

#if 0 && XAPIAN_AT_LEAST(1,3,5)
    // Tryout the Xapian internal method.
    string snippet = xmset.snippet(rawtext, 60);
    std::cerr << "XAPIAN SNIPPET: [" << snippet << "] END SNIPPET\n";
#endif

    // We need the q coefs for individual terms
    unordered_map<string, double> wordcoefs;
    for (const auto& mment : byQ) {
        for (const auto& word : mment.second) {
            wordcoefs[word] = mment.first;
        }
    }

    // Note: getTerms() was already called by qualityTerms, so this is
    // a bit wasteful. I guess that the performance impact is
    // negligible though. To be checked ? We need the highlightdata for the
    // phrase/near groups.
    HighlightData hld;
    if (m_q->m_sd) {
        m_q->m_sd->getTerms(hld);
    }
    LOGABS("abstractFromText: getterms: " << chron.millis() << "mS\n");

    TextSplitABS splitter(rawtext, matchTerms, hld, wordcoefs, ctxwords, TextSplit::TXTS_NONE,
                          m_q->m_snipMaxPosWalk);
    splitter.text_to_words(rawtext);
    LOGABS("abstractFromText: text_to_words: " << chron.millis() << "mS\n");
    splitter.updgroups();

    // Sort the fragments by decreasing weight
    const vector<MatchFragment>& res1 = splitter.getFragments();
    vector<MatchFragment> result(res1.begin(), res1.end());
    if (sortbypage) {
        std::sort(result.begin(), result.end(),
                  [](const MatchFragment& a,
                     const MatchFragment& b) -> bool { 
                      return a.hitpos < b.hitpos; 
                  }
            );
    } else {
        std::sort(result.begin(), result.end(),
                  [](const MatchFragment& a,
                     const MatchFragment& b) -> bool { 
                      return a.coef > b.coef; 
                  }
            );
    }
    vector<int> vpbreaks;
    ndb->getPagePositions(docid, vpbreaks);

    // Build the output snippets array by merging the fragments, their
    // main term and the page positions. 
    unsigned int count = 0;
    for (const auto& entry : result) {
        string frag(fixfrag(rawtext.substr(entry.start, entry.stop - entry.start)));

#ifdef COMPUTE_HLZONES
        // This would need to be modified to take tag parameters
        // instead of the const strings
        static const string starthit("<span style='color: blue;'>");
        static const string endhit("</span>");
        size_t inslen = 0;
        for (const auto& hlzone: entry.hlzones) {
            frag.replace(hlzone.first - entry.start + inslen, 0, starthit);
            inslen += starthit.size();
            frag.replace(hlzone.second - entry.start + inslen, 0, endhit);
            inslen += endhit.size();
        }
#endif
        int page = 0;
        if (vpbreaks.size() > 1) {
            page = ndb->getPageNumberForPosition(vpbreaks, static_cast<int>(entry.hitpos));
            if (page < 0)
                page = 0;
        }
        LOGDEB0("=== FRAGMENT: p. " << page << " Coef: " << entry.coef << ": " << frag << "\n");
        vabs.push_back(Snippet(page, frag, entry.line).setTerm(entry.term));
        if (count++ >= maxtotaloccs)
            break;
    }
    return ABSRES_OK | splitter.getretflags();
}

class TermLineSplitter : public TextSplit {
public:
    TermLineSplitter(const std::string& term)
        : TextSplit(), m_term(term) {
        LOGDEB1("TermLineSplitter: m_term " << m_term << "\n");
    }
    bool takeword(const std::string& _term, size_t, size_t, size_t) override {
        std::string term;
        if (o_index_stripchars) {
            if (!unacmaybefold(_term, term, UNACOP_UNACFOLD)) {
                LOGINFO("PlainToRich::takeword: unac failed for [" << term << "]\n");
                return true;
            }
        }
        LOGDEB1("TermLineSplitter: checking term " << term << "\n");
        if (term == m_term) {
            return false;
        }
        return true;
    }
    void newline(size_t) override {
        m_line++;
    }
    int getline() {
        return m_line;
    }
private:
    int m_line{1};
    std::string m_term;
};

int Query::getFirstMatchLine(const Doc &doc, const std::string& term)
{
    int line = 1;
    TermLineSplitter splitter(term);
    bool ret = splitter.text_to_words(doc.text);
    // The splitter takeword() breaks by returning false as soon as the term is found
    if (ret == false) {
        line = splitter.getline();
    }
    return line;
}

}
