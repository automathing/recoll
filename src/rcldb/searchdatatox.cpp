/* Copyright (C) 2006-2021 J.F.Dockes
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

// Handle translation from rcl's SearchData structures to Xapian Queries

#include "autoconfig.h"

#include <stdio.h>

#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iostream>

#include "xapian.h"

#include "cstr.h"
#include "rcldb.h"
#include "rcldb_p.h"
#include "searchdata.h"
#include "log.h"
#include "smallut.h"
#include "textsplit.h"
#include "unacpp.h"
#include "utf8iter.h"
#include "stoplist.h"
#include "rclconfig.h"
#include "termproc.h"
#include "synfamily.h"
#include "stemdb.h"
#include "expansiondbs.h"
#include "base64.h"
#include "daterange.h"
#include "rclvalues.h"
#include "pathut.h"

using namespace std;

namespace Rcl {

static const int original_term_wqf_booster = 10;

// Expand doc categories and mime type wild card expressions 
//
// Categories are expanded against the configuration, mimetypes
// against the index.
bool SearchData::expandFileTypes(Db &db, vector<string>& tps)
{
    const RclConfig *cfg = db.getConf();
    if (!cfg) {
        LOGFATAL("Db::expandFileTypes: null configuration!!\n");
        return false;
    }
    vector<string> exptps;
    for (const auto& mtype : tps) {
        if (cfg->isMimeCategory(mtype)) {
            vector<string> ctps;
            cfg->getMimeCatTypes(mtype, ctps);
            exptps.insert(exptps.end(), ctps.begin(), ctps.end());
        } else {
            TermMatchResult res;
            string mt = stringtolower(mtype);
            // Expand possible wildcard in mime type, e.g. text/*
            // We set casesens|diacsens to get an equivalent of ixTermMatch()
            db.termMatch(
                Db::ET_WILD|Db::ET_CASESENS|Db::ET_DIACSENS, string(), mt, res, -1, "mtype");
            if (res.entries.empty()) {
                exptps.push_back(mtype);
            } else {
                for (const auto& entry : res.entries) {
                    exptps.push_back(strip_prefix(entry.term));
                }
            }
        }
    }
    sort(exptps.begin(), exptps.end());
    exptps.erase(unique(exptps.begin(), exptps.end()), exptps.end());

    tps = exptps;
    return true;
}

static const char *maxXapClauseMsg = 
    "Maximum Xapian query size exceeded. Increase maxXapianClauses in the configuration. ";
static const char *maxXapClauseCaseDiacMsg = 
    "Or try to use case (C) or diacritics (D) sensitivity qualifiers, or less wildcards ?";


// Walk the clauses list, translate each and add to top Xapian Query
bool SearchData::clausesToQuery(
    Rcl::Db &db, SClType tp, vector<SearchDataClause*>& query, string& reason, void *d)
{
    Xapian::Query xq;
    for (auto& clausep : query) {
#if 0        
        string txt;
        auto clp = dynamic_cast<SearchDataClauseSimple*>(clausep);
        if (clp)
            txt = clp->gettext();
        LOGINF("Clause: tp: " << clausep->getTp() << " txt: [" << txt << "] mods: " <<
                std::hex << clausep->getModifiers() << std::dec << "\n");
#endif
        Xapian::Query nq;
        if (!clausep->toNativeQuery(db, &nq)) {
            LOGERR("SearchData::clausesToQuery: toNativeQuery failed: "
                   << clausep->getReason() << "\n");
            reason += clausep->getReason() + " ";
            return false;
        }       
        if (nq.empty()) {
            LOGDEB0("SearchData::clausesToQuery: skipping empty clause\n");
            continue;
        }
        // If this structure is an AND list, must use AND_NOT for excl clauses.
        // Else this is an OR list, and there can't be excl clauses (checked by
        // addClause())
        Xapian::Query::op op;
        if (tp == SCLT_AND) {
            if (clausep->getexclude()) {
                op =  Xapian::Query::OP_AND_NOT;
            } else {
                if (clausep->getModifiers() & SearchDataClause::SDCM_FILTER) {
                    op =  Xapian::Query::OP_FILTER;
                } else {
                    op =  Xapian::Query::OP_AND;
                }
            }
        } else {
            op = Xapian::Query::OP_OR;
        }
        if (xq.empty()) {
            if (op == Xapian::Query::OP_AND_NOT)
                xq = Xapian::Query(op, Xapian::Query::MatchAll, nq);
            else 
                xq = nq;
        } else {
            xq = Xapian::Query(op, xq, nq);
        }
        if (int(xq.get_length()) >= getMaxCl()) {
            LOGERR("" << maxXapClauseMsg << "\n");
            m_reason += maxXapClauseMsg;
            if (!o_index_stripchars)
                m_reason += maxXapClauseCaseDiacMsg;
            return false;
        }
    }

    LOGDEB0("SearchData::clausesToQuery: got " << xq.get_length()<<" clauses\n");

    if (xq.empty())
        xq = Xapian::Query::MatchAll;

    *((Xapian::Query *)d) = xq;
    return true;
}

static void processdaterange(Rcl::Db& db, Xapian::Query& xq, DateInterval& dates, bool isbr = false)
{
    // If one of the extremities is unset, compute db extremas
    if (dates.y1 == 0 || dates.y2 == 0) {
        int minyear = 1970, maxyear = 2100;
        if (!db.maxYearSpan(&minyear, &maxyear)) {
            LOGERR("Can't retrieve index min/max dates\n");
            //whatever, go on.
        }

        if (dates.y1 == 0) {
            dates.y1 = minyear;
            dates.m1 = 1;
            dates.d1 = 1;
        }
        if (dates.y2 == 0) {
            dates.y2 = maxyear;
            dates.m2 = 12;
            dates.d2 = 31;
        }
    }
    LOGDEB("Db::toNativeQuery: " << (isbr?"birtime":"date") << " interval: " << dates.y1 <<
           "-" << dates.m1 << "-" << dates.d1 << "/" <<
           dates.y2 << "-" << dates.m2 << "-" << dates.d2 << "\n");
    Xapian::Query dq;
#ifdef EXT4_BIRTH_TIME
    if (isbr) {
        dq = brdate_range_filter(dates.y1, dates.m1, dates.d1, dates.y2, dates.m2, dates.d2);
    } else
#endif
    {
        dq = date_range_filter(dates.y1, dates.m1, dates.d1, dates.y2, dates.m2, dates.d2);
    }
    if (dq.empty()) {
        LOGINFO("Db::toNativeQuery: date filter is empty\n");
    }
    // If no probabilistic query is provided then promote the daterange
    // filter to be THE query instead of filtering an empty query.
    if (xq.empty()) {
        LOGINFO("Db::toNativeQuery: proba query is empty\n");
        xq = dq;
    } else {
        xq = Xapian::Query(Xapian::Query::OP_FILTER, xq, dq);
    }
}

bool SearchData::toNativeQuery(Rcl::Db &db, void *d)
{
    LOGDEB("SearchData::toNativeQuery: stemlang [" << m_stemlang << "]\n");
    m_reason.erase();

    db.getConf()->getConfParam("maxTermExpand", &m_maxexp);
    db.getConf()->getConfParam("maxXapianClauses", &m_maxcl);
    m_autocasesens = true;
    db.getConf()->getConfParam("autocasesens", &m_autocasesens);
    m_autodiacsens = false;
    db.getConf()->getConfParam("autodiacsens", &m_autodiacsens);

    simplify();
    // Walk the clause list translating each in turn and building the 
    // Xapian query tree
    Xapian::Query xq;
    if (!clausesToQuery(db, m_tp, m_query, m_reason, &xq)) {
        LOGERR("SearchData::toNativeQuery: clausesToQuery failed. reason: " << m_reason << "\n");
        return false;
    }

    if (m_haveDates) {
        processdaterange(db, xq, m_dates);
    }

#ifdef EXT4_BIRTH_TIME
    //handle birtime
    if (m_haveBrDates) {
        processdaterange(db, xq, m_brdates, true);
    }
#endif

    if (m_minSize != -1 || m_maxSize != -1) {
        Xapian::Query sq;
        string min = lltodecstr(m_minSize);
        string max = lltodecstr(m_maxSize);
        if (m_minSize == -1) {
            string value(max);
            leftzeropad(value, 12);
            sq = Xapian::Query(Xapian::Query::OP_VALUE_LE, VALUE_SIZE, value);
        } else if (m_maxSize == -1) {
            string value(min);
            leftzeropad(value, 12);
            sq = Xapian::Query(Xapian::Query::OP_VALUE_GE, VALUE_SIZE, value);
        } else {
            string minvalue(min);
            leftzeropad(minvalue, 12);
            string maxvalue(max);
            leftzeropad(maxvalue, 12);
            sq = Xapian::Query(Xapian::Query::OP_VALUE_RANGE, VALUE_SIZE, minvalue, maxvalue);
        }
        
        // If no probabilistic query is provided then promote the
        // filter to be THE query instead of filtering an empty query.
        if (xq.empty()) {
            LOGINFO("Db::toNativeQuery: proba query is empty\n");
            xq = sq;
        } else {
            xq = Xapian::Query(Xapian::Query::OP_FILTER, xq, sq);
        }
    }

    // Add the autophrase if any
    if (m_autophrase) {
        Xapian::Query apq;
        if (m_autophrase->toNativeQuery(db, &apq)) {
            xq = xq.empty() ? apq : Xapian::Query(Xapian::Query::OP_AND_MAYBE, xq, apq);
        }
    }

    // Add the file type filtering clause if any
    if (!m_filetypes.empty()) {
        expandFileTypes(db, m_filetypes);
        
        Xapian::Query tq;
        for (const auto& ft : m_filetypes) {
            string term = wrap_prefix(mimetype_prefix) + ft;
            LOGDEB0("Adding file type term: [" << term << "]\n");
            tq = tq.empty() ? Xapian::Query(term) :
                Xapian::Query(Xapian::Query::OP_OR, tq, Xapian::Query(term));
        }
        xq = xq.empty() ? tq : Xapian::Query(Xapian::Query::OP_FILTER, xq, tq);
    }

    // Add the neg file type filtering clause if any
    if (!m_nfiletypes.empty()) {
        expandFileTypes(db, m_nfiletypes);
        
        Xapian::Query tq;
        for (const auto& ft : m_nfiletypes) {
            string term = wrap_prefix(mimetype_prefix) + ft;
            LOGDEB0("Adding negative file type term: [" << term << "]\n");
            tq = tq.empty() ? Xapian::Query(term) : 
                Xapian::Query(Xapian::Query::OP_OR, tq, Xapian::Query(term));
        }
        xq = xq.empty() ? tq : Xapian::Query(Xapian::Query::OP_AND_NOT, xq, tq);
    }

    *((Xapian::Query *)d) = xq;
    return true;
}

// Splitter for breaking a user string into simple terms and
// phrases. This is for parts of the user entry which would appear as
// a single word because there is no white space inside, but are
// actually multiple terms to rcldb (ie term1,term2). Still, most of
// the time, the result of our splitting will be a single term.
class TextSplitQ : public TextSplitP {
public:
    TextSplitQ(int flags, TermProc *prc)
        : TextSplitP(prc, flags), m_nostemexp(false) {
    }

    bool takeword(const std::string &term, size_t pos, size_t bs, size_t be) override {
        // Check if the first letter is a majuscule in which
        // case we do not want to do stem expansion. Need to do this
        // before unac of course...
        m_nostemexp = unaciscapital(term);

        return TextSplitP::takeword(term, pos, bs, be);
    }
    virtual bool discarded(const std::string &term, size_t, size_t, size_t, DiscardReason reason)
        override {
        m_problemterm = term;
        return true;
    }

    bool nostemexp() const {
        return m_nostemexp;
    }
    std::string getproblemterm() {
        return m_problemterm;
    }
private:
    bool m_nostemexp;
    std::string m_problemterm;
};

class TermProcQ : public TermProc {
public:
    TermProcQ() : TermProc(nullptr), m_alltermcount(0), m_lastpos(0), m_ts(nullptr) {}

    // We need a ref to the splitter (only it knows about orig term
    // capitalization for controlling stemming. The ref can't be set
    // in the constructor because the splitter is not built yet when
    // we are born (chicken and egg).
    void setTSQ(const TextSplitQ *ts) {
        m_ts = ts;
    }
    
    bool takeword(const std::string &term, size_t _pos, size_t, size_t be) override {
        m_alltermcount++;
        int ipos = static_cast<int>(_pos);
        if (m_lastpos < ipos)
            m_lastpos = ipos;
        bool noexpand = be ? m_ts->nostemexp() : true;
        LOGDEB1("TermProcQ::takeword: pushing [" << term << "] pos " <<
                pos << " noexp " << noexpand << "\n");
        if (m_terms[ipos].size() < term.size()) {
            m_terms[ipos] = term;
            m_nste[ipos] = noexpand;
        }
        return true;
    }

    bool flush() override {
        for (const auto& entry : m_terms) {
            m_vterms.push_back(entry.second);
            m_vnostemexps.push_back(m_nste[entry.first]);
        }
        return true;
    }

    int alltermcount() const {
        return m_alltermcount;
    }
    int lastpos() const {
        return m_lastpos;
    }
    const vector<string>& terms() {
        return m_vterms;
    }
    const vector<bool>& nostemexps() {
        return m_vnostemexps;
    }
private:
    // Count of terms including stopwords: this is for adjusting
    // phrase/near slack
    int m_alltermcount; 
    int m_lastpos;
    const TextSplitQ *m_ts;
    vector<string> m_vterms;
    vector<bool>   m_vnostemexps;
    map<int, string> m_terms;
    map<int, bool> m_nste;
};

static const vector<CharFlags> expandModStrings{
    {SearchDataClause::SDCM_NOSTEMMING, "nostemming"},
    {SearchDataClause::SDCM_ANCHORSTART, "anchorstart"},
    {SearchDataClause::SDCM_ANCHOREND, "anchorend"},
    {SearchDataClause::SDCM_CASESENS, "casesens"},
    {SearchDataClause::SDCM_DIACSENS, "diacsens"},
    {SearchDataClause::SDCM_NOTERMS, "noterms"},
    {SearchDataClause::SDCM_NOSYNS, "nosyns"},
    {SearchDataClause::SDCM_PATHELT, "pathelt"},
    {SearchDataClause::SDCM_FILTER, "filter"},
    {SearchDataClause::SDCM_EXPANDPHRASE, "expandphrase"},
    {SearchDataClause::SDCM_NOWILDEXP, "nowildexp"},
};

/** Expand term into term list, using appropriate mode: stem, wildcards, 
 *  diacritics... 
 *
 * @param mods stem expansion, case and diacritics sensitivity control.
 * @param term input single word
 * @param oexp output expansion list
 * @param sterm output original input term if there were no wildcards
 * @param prefix field prefix in index. We could recompute it, but the caller
 *  has it already. Used in the simple case where there is nothing to expand, 
 *  and we just return the prefixed term (else Db::termMatch deals with it).
 * @param multiwords it may happen that synonym processing results in multi-word
 *   expansions which should be processed as phrases.
 */
bool SearchDataClauseSimple::expandTerm(Rcl::Db &db, 
                                        string& ermsg, int mods, 
                                        const string& term, 
                                        vector<string>& oexp, string &sterm,
                                        const string& prefix,
                                        vector<string>* multiwords
    )
{
    LOGDEB0("expandTerm: mods: [" << flagsToString(expandModStrings, mods) <<
            "] fld [" << m_field << "] trm [" << term << "] lang [" <<
            getStemLang() << "]\n");
    sterm.clear();
    oexp.clear();
    if (term.empty())
        return true;

    if (mods & SDCM_PATHELT) {
        // Path element are so special. Only wildcards, and they are
        // case-sensitive.
        mods |= SDCM_NOSTEMMING|SDCM_CASESENS|SDCM_DIACSENS|SDCM_NOSYNS;
    }

    bool maxexpissoft = false;
    int maxexpand = getSoftMaxExp();
    if (maxexpand != -1) {
        maxexpissoft = true;
    } else {
        maxexpand = getMaxExp();
    }

    bool haswild = term.find_first_of(cstr_minwilds) != string::npos;

    // If there are no wildcards, add term to the list of user-entered terms
    bool dowildexp = !getNoWildExp() && haswild;
    if (!dowildexp) {
        m_hldata.uterms.insert(term);
        sterm = term;
    }
    // No stem expansion if there are wildcards (even if nowildexp) or if prevented by caller
    bool nostemexp = (mods & SDCM_NOSTEMMING) != 0;
    if (haswild || getStemLang().empty()) {
        LOGDEB2("expandTerm: found wildcards or stemlang empty: no exp\n");
        nostemexp = true;
    }

    bool diac_sensitive = (mods & SDCM_DIACSENS) != 0;
    bool case_sensitive = (mods & SDCM_CASESENS) != 0;
    bool synonyms = (mods & SDCM_NOSYNS) == 0;
    bool pathelt = (mods & SDCM_PATHELT) != 0;
    
    // noexpansion can be modified further down by possible case/diac expansion
    bool noexpansion = nostemexp && !dowildexp && !synonyms; 

    if (o_index_stripchars) {
        diac_sensitive = case_sensitive = false;
    } else {
        // If we are working with a raw index, apply the rules for case and 
        // diacritics sensitivity.

        // If any character has a diacritic, we become
        // diacritic-sensitive. Note that the way that the test is
        // performed (conversion+comparison) will automatically ignore
        // accented characters which are actually a separate letter
        if (getAutoDiac() && unachasaccents(term)) {
            LOGDEB0("expandTerm: term has accents -> diac-sensitive\n");
            diac_sensitive = true;
        }

        // If any character apart the first is uppercase, we become
        // case-sensitive.  The first character is reserved for
        // turning off stemming. You need to use a query language
        // modifier to search for Floor in a case-sensitive way.
        Utf8Iter it(term);
        it++;
        if (getAutoCase() && unachasuppercase(term.substr(it.getBpos()))) {
            LOGDEB0("expandTerm: term has uppercase -> case-sensitive\n");
            case_sensitive = true;
        }

        // If we are sensitive to case or diacritics turn stemming off
        if (diac_sensitive || case_sensitive) {
            LOGDEB0("expandTerm: diac or case sens set -> stemexpand and synonyms off\n");
            nostemexp = true;
            synonyms = false;
        }

        if (!case_sensitive || !diac_sensitive)
            noexpansion = false;
    }


    if (!m_exclude && noexpansion) {
        oexp.push_back(prefix + term);
        m_hldata.terms[term] = term;
        LOGDEB("ExpandTerm: noexpansion: final: "<<stringsToString(oexp)<< "\n");
        return true;
    } 

    int termmatchsens = 0;
    if (case_sensitive)
        termmatchsens |= Db::ET_CASESENS;
    if (diac_sensitive)
        termmatchsens |= Db::ET_DIACSENS;
    if (synonyms)
        termmatchsens |= Db::ET_SYNEXP;
    if (pathelt) 
        termmatchsens |= Db::ET_PATHELT;
    Db::MatchType mtyp = dowildexp ? Db::ET_WILD : nostemexp ? Db::ET_NONE : Db::ET_STEM;
    TermMatchResult res;
    if (!db.termMatch(mtyp | termmatchsens, getStemLang(),
                      term, res, maxexpand, m_field, multiwords)) {
        // Let it go through
    }

    // Term match entries to vector of terms
    if (int(res.entries.size()) >= maxexpand && !maxexpissoft) {
        ermsg = "Maximum term expansion size exceeded."
            " Maybe use case/diacritics sensitivity or increase maxTermExpand.";
        return false;
    }
    for (const auto& entry : res.entries) {
        oexp.push_back(entry.term);
    }
    // If the term does not exist at all in the db, the return from
    // termMatch() is going to be empty, which is not what we want (we
    // would then compute an empty Xapian query)
    if (oexp.empty())
        oexp.push_back(prefix + term);

    // Remember the uterm-to-expansion links
    if (!m_exclude) {
        for (const auto& entry : oexp) {
            m_hldata.terms[strip_prefix(entry)] = term;
        }
    }
    // Remember the terms generated trough spelling approximation
    m_hldata.spellexpands.insert(m_hldata.spellexpands.end(),
                                 res.fromspelling.begin(), res.fromspelling.end());
    LOGDEB("ExpandTerm: final: " << stringsToString(oexp) << "\n");
    return true;
}

static void prefix_vector(vector<string>& v, const string& prefix)
{
    for (auto& elt : v) {
        elt = prefix + elt;
    }
}

void SearchDataClauseSimple::processSimpleSpan(
    Rcl::Db &db, string& ermsg, const string& span, int mods, void *pq)
{
    vector<Xapian::Query>& pqueries(*(vector<Xapian::Query>*)pq);
    LOGDEB0("StringToXapianQ::processSimpleSpan: [" << span << "] mods 0x"
           << (unsigned int)mods << "\n");

    string prefix;
    const FieldTraits *ftp;
    if (!m_field.empty() && db.fieldToTraits(m_field, &ftp, true)) {
        if (ftp->noterms)
            addModifier(SDCM_NOTERMS); // Don't add terms to highlight data
        prefix = wrap_prefix(ftp->pfx);
    }

    vector<string> exp;  
    string sterm; // dumb version of user term
    vector<string> multiwords;
    static string wildcardchars{"*?[]"};
    // Special case: in case nowildexp is set and we get a single char (because it's in
    // indexedpunctuation probably), check if it's a wildcard and don't do expandTerm at
    // all. Simpler than dealing with the case inside expandTerm.
    if (getNoWildExp() && span.size() == 1 && wildcardchars.find(span[0]) != string::npos) {
        exp.push_back(span);
        sterm = span;
    } else {
        if (!expandTerm(db, ermsg, mods, span, exp, sterm, prefix, &multiwords)) {
            LOGINF("processSimpleSpan: expandterm failed\n");
            return;
        }
    }
    
    // Set up the highlight data. No prefix should go in there
    if (!m_exclude) {
        for (const auto& term : exp) {
            HighlightData::TermGroup tg;
            tg.term = term.substr(prefix.size());
            tg.grpsugidx =  m_hldata.ugroups.size() - 1;
            m_hldata.index_term_groups.push_back(tg);
        }
    }
    
    // Push either term or OR of stem-expanded set
    Xapian::Query xq(Xapian::Query::OP_OR, exp.begin(), exp.end());
    m_curcl += exp.size();

    // If sterm (simplified original user term) is not null, give it a
    // relevance boost. We do this even if no expansion occurred (else
    // the non-expanded terms in a term list would end-up with even
    // less wqf). This does not happen if there are wildcards anywhere
    // in the search.
    // We normally boost the original term in the stem expansion list. Don't
    // do it if there are wildcards anywhere, this would skew the results. Also
    // no need to do it if there was no expansion.
    bool doBoostUserTerm = 
        (m_parentSearch && !m_parentSearch->haveWildCards()) || 
        (nullptr == m_parentSearch && !m_haveWildCards);
    if (exp.size() > 1 && doBoostUserTerm && !sterm.empty()) {
        xq = Xapian::Query(Xapian::Query::OP_OR, xq,
                           Xapian::Query(prefix+sterm, original_term_wqf_booster));
    }

    // Push phrases for the multi-word expansions
    for (const auto& mw : multiwords) {
        vector<string> phr;
        // We just do a basic split to keep things a bit simpler here
        // (no textsplit). This means though that no punctuation is
        // allowed in multi-word synonyms.
        stringToTokens(mw, phr);
        if (!prefix.empty())
            prefix_vector(phr, prefix);
        xq = Xapian::Query(Xapian::Query::OP_OR, xq, 
                           Xapian::Query(Xapian::Query::OP_PHRASE, 
                                         phr.begin(), phr.end()));
        m_curcl++;
    }

    pqueries.push_back(xq);
}

// User entry element had several terms: transform into a PHRASE or
// NEAR xapian query, the elements of which can themselves be OR
// queries if the terms get expanded by stemming or wildcards (we
// don't do stemming for PHRASE though)
void SearchDataClauseSimple::processPhraseOrNear(
    Rcl::Db &db, string& ermsg, TermProcQ *splitData, int mods, void *pq, bool useNear, int slack)
{
    vector<Xapian::Query> &pqueries(*(vector<Xapian::Query>*)pq);
    Xapian::Query::op op = useNear ? Xapian::Query::OP_NEAR : Xapian::Query::OP_PHRASE;
    vector<Xapian::Query> orqueries;
    vector<vector<string> >groups;

    bool useidxsynonyms = db.getSynGroups().getpath() == db.getConf()->getIdxSynGroupsFile();
    
    string prefix;
    const FieldTraits *ftp;
    if (!m_field.empty() && db.fieldToTraits(m_field, &ftp, true)) {
        prefix = wrap_prefix(ftp->pfx);
    }

    if (mods & Rcl::SearchDataClause::SDCM_ANCHORSTART) {
        orqueries.push_back(Xapian::Query(prefix + start_of_field_term));
    }

    // Max term count in a multiword. See below comment about slack adjustment
    int maxmwordlen = 0;
    // Go through the list and perform stem/wildcard expansion for each element
    auto nxit = splitData->nostemexps().begin();
    for (auto it = splitData->terms().begin(); it != splitData->terms().end(); it++, nxit++) {
        LOGDEB0("ProcessPhrase: processing [" << *it << "]\n");
        // Adjust when we do stem expansion. Not if disabled by caller, not inside phrases.
        bool nostemexp = *nxit ||
            (op == Xapian::Query::OP_PHRASE && !o_expand_phrases &&
             !(mods & Rcl::SearchDataClause::SDCM_EXPANDPHRASE));
        int lmods = mods;
        if (nostemexp)
            lmods |= SearchDataClause::SDCM_NOSTEMMING;
        string sterm;
        vector<string> exp;
        vector<string> multiwords;
        if (!expandTerm(db, ermsg, lmods, *it, exp, sterm, prefix, &multiwords))
            return;

        // Note: because of how expandTerm works, the multiwords can only come from the synonyms
        // expansion, which means that, if idxsynonyms is set, they have each been indexed as a
        // single term. So, if idxsynonyms is set, and is the current active synonyms file, we just
        // add them to the expansion. We have to increase the slack in this case, which will also
        // apply to single-word expansions and cause false matches, but this is better than missing
        // multiword matches (this is because, even if the multiword term was issued at the position
        // of its first word during indexing, the position of the first term after the multiword is
        // still increased by the number of words in the multiword).
        if (!multiwords.empty() && useidxsynonyms) {
            for (const auto& mword: multiwords) {
                int cnt = std::count(mword.begin(), mword.end(), ' ') + 1;
                if (cnt > maxmwordlen)
                    maxmwordlen = cnt;
            }
            exp.insert(exp.end(), multiwords.begin(), multiwords.end());
        }

        LOGDEB0("ProcessPhraseOrNear: exp size " << exp.size() << ", exp: " <<
                stringsToString(exp) << "\n");
        // groups is used for highlighting, we don't want prefixes in there.
        vector<string> noprefs;
        for (const auto& prefterm : exp) {
            noprefs.push_back(prefterm.substr(prefix.size()));
        }
        groups.push_back(noprefs);
        orqueries.push_back(Xapian::Query(Xapian::Query::OP_OR, exp.begin(), exp.end()));
        m_curcl += exp.size();
        if (m_curcl >= getMaxCl())
            return;
    }

    if (mods & Rcl::SearchDataClause::SDCM_ANCHOREND) {
        orqueries.push_back(Xapian::Query(prefix + end_of_field_term));
    }

    // Generate an appropriate PHRASE/NEAR query with adjusted slack
    // For phrases, give a relevance boost like we do for original terms
    LOGDEB2("PHRASE/NEAR:  alltermcount " << splitData->alltermcount() <<
            " lastpos " << splitData->lastpos() << "\n");
    if (maxmwordlen > 1) {
        slack += maxmwordlen - 1;
    }
    Xapian::Query xq(op, orqueries.begin(), orqueries.end(),
                     static_cast<int>(orqueries.size()) + slack);
    if (op == Xapian::Query::OP_PHRASE)
        xq = Xapian::Query(Xapian::Query::OP_SCALE_WEIGHT, xq, original_term_wqf_booster);
    pqueries.push_back(xq);

    // Insert the search groups and slacks in the highlight data, with
    // a reference to the user entry that generated them:
    if (!m_exclude) {
        HighlightData::TermGroup tg;
        tg.orgroups = groups;
        tg.slack = slack;
        tg.grpsugidx =  m_hldata.ugroups.size() - 1;
        tg.kind = (op == Xapian::Query::OP_PHRASE) ?
            HighlightData::TermGroup::TGK_PHRASE :
            HighlightData::TermGroup::TGK_NEAR;
        m_hldata.index_term_groups.push_back(tg);
    }
}

// Trim string beginning with ^ or ending with $ and convert to flags
static int stringToMods(string& s)
{
    int mods = 0;
    // Check for an anchored search
    trimstring(s);
    if (s.length() > 0 && s[0] == '^') {
        mods |= Rcl::SearchDataClause::SDCM_ANCHORSTART;
        s.erase(0, 1);
    }
    if (s.length() > 0 && s[s.length()-1] == '$') {
        mods |= Rcl::SearchDataClause::SDCM_ANCHOREND;
        s.erase(s.length()-1);
    }
    return mods;
}

/** 
 * Turn user entry string (NOT raw query language, but possibly the contents of a phrase/near
 * clause out of the parser) into a list of Xapian queries.
 * We just separate words and phrases, and do wildcard and stem expansion,
 *
 * This is used to process data entered into an OR/AND/NEAR/PHRASE field of
 * the GUI (in the case of NEAR/PHRASE, clausedist adds dquotes to the user
 * entry).
 *
 * This appears awful, and it would seem that the split into
 * terms/phrases should be performed in the upper layer so that we
 * only receive pure term or near/phrase pure elements here, but in
 * fact there are things that would appear like terms to naive code,
 * and which will actually may be turned into phrases (ie: tom-jerry),
 * in a manner which intimately depends on the index implementation,
 * so that it makes sense to process this here.
 *
 * The final list contains one query for each term or phrase
 *   - Elements corresponding to a stem-expanded part are an OP_OR
 *     composition of the stem-expanded terms (or a single term query).
 *   - Elements corresponding to phrase/near are an OP_PHRASE/NEAR
 *     composition of the phrase terms (no stem expansion in this case)
 * @return the subquery count (either or'd stem-expanded terms or phrase word
 *   count)
 */
bool SearchDataClauseSimple::processUserString(
    Rcl::Db &db, const string &iq, string &ermsg, string &pbterm, void *pq, int slack0, bool useNear)
{
    vector<Xapian::Query> &pqueries(*(vector<Xapian::Query>*)pq);
    int mods = m_modifiers;

    LOGDEB("StringToXapianQ:pUS:: qstr [" << iq << "] fld [" << m_field <<
           "] mods 0x"<<mods<< " slack " << slack0 << " near " << useNear <<"\n");
    ermsg.erase();
    m_curcl = 0;
    const StopList stops = db.getStopList();

    // Simple whitespace-split input into user-level words and
    // double-quoted phrases: word1 word2 "this is a phrase". 
    //
    // The text splitter may further still decide that the resulting
    // "words" are really phrases, this depends on separators:
    // [paul@dom.net] would still be a word (span), but [about-me]
    // will probably be handled as a phrase.
    vector<string> phrases;
    TextSplit::stringToStrings(iq, phrases);

    // Process each element: textsplit into terms, handle stem/wildcard 
    // expansion and transform into an appropriate Xapian::Query
    try {
        for (auto& wordorphrase : phrases) {
            LOGDEB0("strToXapianQ: phrase/word: [" << wordorphrase << "]\n");
            int slack = slack0;
            // Anchoring modifiers
            int amods = stringToMods(wordorphrase);
            int terminc = amods != 0 ? 1 : 0;
            mods |= amods;
            // If there are multiple spans in this element, including
            // at least one composite, we have to increase the slack
            // else a phrase query including a span would fail. 
            // Ex: "term0@term1 term2" is onlyspans-split as:
            //   0 term0@term1             0   12
            //   2 term2                  13   18
            // The position of term2 is 2, not 1, so a phrase search
            // would fail.
            // We used to do  word split, searching for 
            // "term0 term1 term2" instead, which may have worse 
            // performance, but will succeed.
            // We now adjust the phrase/near slack by comparing the term count
            // and the last position

            // The term processing pipeline:
            // split -> [unac/case ->] stops -> store terms
            TermProcQ tpq;
            TermProc *nxt = &tpq;
            TermProcStop tpstop(nxt, stops); nxt = &tpstop;
            //TermProcCommongrams tpcommon(nxt, stops); nxt = &tpcommon;
            //tpcommon.onlygrams(true);
            TermProcPrep tpprep(nxt);
            if (o_index_stripchars)
                nxt = &tpprep;

            int txtsplitflags = TextSplit::TXTS_ONLYSPANS;
            if (!getNoWildExp()) {
                txtsplitflags |= TextSplit::TXTS_KEEPWILD;
            }
            TextSplitQ splitter(txtsplitflags, nxt);
            tpq.setTSQ(&splitter);
            splitter.text_to_words(wordorphrase);
            if (!splitter.getproblemterm().empty()) {
                pbterm = splitter.getproblemterm();
            }
            slack += tpq.lastpos() - int(tpq.terms().size()) + 1;

            LOGDEB0("strToXapianQ: termcount: " << tpq.terms().size() << "\n");
            switch (tpq.terms().size() + terminc) {
            case 0: 
                continue;// ??
            case 1: {
                int lmods = mods;
                if (tpq.nostemexps().front())
                    lmods |= SearchDataClause::SDCM_NOSTEMMING;
                if (!m_exclude) {
                    m_hldata.ugroups.push_back(tpq.terms());
                }
                processSimpleSpan(db, ermsg, tpq.terms().front(), lmods, &pqueries);
            }
                break;
            default:
                if (!m_exclude) {
                    m_hldata.ugroups.push_back(tpq.terms());
                }
                processPhraseOrNear(db, ermsg, &tpq, mods, &pqueries, useNear, slack);
            }
            if (m_curcl >= getMaxCl()) {
                ermsg = maxXapClauseMsg;
                if (!o_index_stripchars)
                    ermsg += maxXapClauseCaseDiacMsg;
                break;
            }
        }
    } catch (const Xapian::Error &e) {
        ermsg = e.get_msg();
    } catch (const string &s) {
        ermsg = s;
    } catch (const char *s) {
        ermsg = s;
    } catch (...) {
        ermsg = "Caught unknown exception";
    }
    if (!ermsg.empty()) {
        LOGERR("stringToXapianQueries: " << ermsg << "\n");
        return false;
    }
    return true;
}

// Translate a simple OR or AND search clause. 
bool SearchDataClauseSimple::toNativeQuery(Rcl::Db &db, void *p)
{
    LOGDEB("SearchDataClauseSimple::toNativeQuery: fld [" << m_field <<
           "] val [" << m_text << "] stemlang [" << getStemLang() << "]\n");

    // Transform (in)equalities into a range query
    switch (getrel()) {
    case REL_EQUALS:
    {
        SearchDataClauseRange cl(*this, gettext(), gettext());
        bool ret = cl.toNativeQuery(db, p);
        m_reason = cl.getReason();
        return ret;
    }
    case REL_LT: case REL_LTE:
    {
        SearchDataClauseRange cl(*this, "", gettext());
        bool ret = cl.toNativeQuery(db, p);
        m_reason = cl.getReason();
        return ret;
    }
    case REL_GT: case REL_GTE:
    {
        SearchDataClauseRange cl(*this, gettext(), "");
        bool ret = cl.toNativeQuery(db, p);
        m_reason = cl.getReason();
        return ret;
    }
    default:
        break;
    }
        
    Xapian::Query *qp = (Xapian::Query *)p;
    *qp = Xapian::Query();

    Xapian::Query::op op;
    switch (m_tp) {
    case SCLT_AND: op = Xapian::Query::OP_AND; break;
    case SCLT_OR: op = Xapian::Query::OP_OR; break;
    default:
        LOGERR("SearchDataClauseSimple: bad m_tp " << m_tp << "\n");
        m_reason = "Internal error";
        return false;
    }

    vector<Xapian::Query> pqueries;
    std::string pbterm;
    if (!processUserString(db, m_text, m_reason, pbterm, &pqueries))
        return false;
    if (pqueries.empty()) {
        LOGDEB("SearchDataClauseSimple: " << m_text << " resolved to null query\n");
        if (!pbterm.empty()) 
            m_reason = string("Resolved to null query. Problem term : [" + pbterm + string("]"));
        else
            m_reason = string("Resolved to null query. Term too long ? : [" + m_text + string("]"));
        return false;
    }

    *qp = Xapian::Query(op, pqueries.begin(), pqueries.end());
    if  (m_weight != 1.0) {
        *qp = Xapian::Query(Xapian::Query::OP_SCALE_WEIGHT, *qp, m_weight);
    }
    return true;
}

// Translate a range clause. This only works if a Xapian value slot
// was attributed to the field.
bool SearchDataClauseRange::toNativeQuery(Rcl::Db &db, void *p)
{
    LOGDEB("SearchDataClauseRange::toNativeQuery: " << m_field <<
           " :[" << m_text << ".." << m_t2 << "]\n");
    Xapian::Query *qp = (Xapian::Query *)p;
    *qp = Xapian::Query();

    if (m_field.empty() || (m_text.empty() && m_t2.empty())) {
        m_reason = "Range clause needs a field and a value";
        return false;
    }

    // Get the value number for the field from the configuration
    const FieldTraits *ftp;
    if (!db.fieldToTraits(m_field, &ftp, true)) {
        m_reason = string("field ") + m_field + " not found in configuration";
        return false;
    }
    if (ftp->valueslot == 0) {
        m_reason = string("No value slot specified in configuration for field ") + m_field;
        return false;
    }
    LOGDEB("SearchDataClauseRange: value slot " << ftp->valueslot << endl);
    // Build Xapian VALUE query.
    string errstr;
    try {
        if (m_text.empty()) {
            *qp = Xapian::Query(Xapian::Query::OP_VALUE_LE,
                                ftp->valueslot, convert_field_value(*ftp, m_t2));
        } else if (m_t2.empty()) {
            *qp = Xapian::Query(Xapian::Query::OP_VALUE_GE, ftp->valueslot,
                                convert_field_value(*ftp, m_text));
        } else {
            *qp = Xapian::Query(Xapian::Query::OP_VALUE_RANGE, ftp->valueslot,
                                convert_field_value(*ftp, m_text),
                                convert_field_value(*ftp, m_t2));
        }
    }
    XCATCHERROR(errstr);
    if (!errstr.empty()) {
        LOGERR("SearchDataClauseRange: range query creation failed for slot "<<ftp->valueslot<<"\n");
        m_reason = "Range query creation failed\n";
        *qp = Xapian::Query();
        return false;
    }
    return true;
}

// Translate a FILENAME search clause. This always comes
// from a "filename" search from the gui or recollq. A query language
// "filename:"-prefixed field will not go through here, but through
// the generic field-processing code.
//
// We do not split the entry any more (used to do some crazy thing
// about expanding multiple fragments in the past). We just take the
// value blanks and all and expand this against the indexed unsplit
// file names
bool SearchDataClauseFilename::toNativeQuery(Rcl::Db &db, void *p)
{
    Xapian::Query *qp = (Xapian::Query *)p;
    *qp = Xapian::Query();

    int maxexp = getSoftMaxExp();
    if (maxexp == -1)
        maxexp = getMaxExp();

    vector<string> names;
    db.filenameWildExp(m_text, names, maxexp);
    *qp = Xapian::Query(Xapian::Query::OP_OR, names.begin(), names.end());

    if (m_weight != 1.0) {
        *qp = Xapian::Query(Xapian::Query::OP_SCALE_WEIGHT, *qp, m_weight);
    }
    return true;
}

// Translate a dir: path filtering clause. See comments in .h
bool SearchDataClausePath::toNativeQuery(Rcl::Db &db, void *p)
{
    LOGDEB("SearchDataClausePath::toNativeQuery: [" << m_text << "]\n");
    Xapian::Query *qp = (Xapian::Query *)p;
    *qp = Xapian::Query();

    string ltext;
#ifdef _WIN32
    // Windows file names are case-insensitive, so we lowercase (same as when indexing)
    unacmaybefold(m_text, ltext, UNACOP_FOLD);
#else
    ltext = m_text;
#endif

    if (ltext.empty()) {
        LOGERR("SearchDataClausePath: empty path??\n");
        m_reason = "Empty path ?";
        return false;
    }

    vector<Xapian::Query> orqueries;

    if (path_isabsolute(ltext))
        orqueries.push_back(Xapian::Query(wrap_prefix(pathelt_prefix)));
    else
        ltext = path_tildexpand(ltext);

    vector<string> vpath;
    stringToTokens(ltext, vpath, "/");

    for (const auto& pathelt : vpath) {
        string sterm;
        vector<string> exp;
        if (!expandTerm(
                db, m_reason, SDCM_PATHELT, pathelt, exp, sterm, wrap_prefix(pathelt_prefix))) {
            return false;
        }
        LOGDEB0("SDataPath::toNative: exp size " << exp.size() << ". Exp: " <<
                stringsToString(exp) << "\n");
        if (exp.size() == 1)
            orqueries.push_back(Xapian::Query(exp[0]));
        else 
            orqueries.push_back(Xapian::Query(Xapian::Query::OP_OR, exp.begin(), exp.end()));
        m_curcl += exp.size();
        if (m_curcl >= getMaxCl())
            return false;
    }

    *qp = Xapian::Query(Xapian::Query::OP_PHRASE, orqueries.begin(), orqueries.end());

    if (m_weight != 1.0) {
        *qp = Xapian::Query(Xapian::Query::OP_SCALE_WEIGHT, *qp, m_weight);
    }
    return true;
}

// Translate NEAR or PHRASE clause. 
bool SearchDataClauseDist::toNativeQuery(Rcl::Db &db, void *p)
{
    LOGDEB("SearchDataClauseDist::toNativeQuery\n");

    Xapian::Query *qp = (Xapian::Query *)p;
    *qp = Xapian::Query();

    vector<Xapian::Query> pqueries;

    // We produce a single phrase out of the user entry then use processUserString() to lowercase
    // and simplify the phrase terms etc. This will result into a single (complex) Xapian::Query.
    if (m_text.find('\"') != string::npos) {
        m_text = neutchars(m_text, "\"");
    }
    string s = cstr_dquote + m_text + cstr_dquote;
    bool useNear = (m_tp == SCLT_NEAR);
    if (!useNear  && !o_expand_phrases && !(m_modifiers & SDCM_EXPANDPHRASE)) {
        // We are a phrase query. Make sure to disable stemming explicitely in case this is a single
        // quoted word because processUserString won't see it as a phrase by itself.
        m_modifiers |= SDCM_NOSTEMMING;
    }
    string pbterm;
    if (!processUserString(db, s, m_reason, pbterm, &pqueries, m_slack, useNear))
        return false;
    if (pqueries.empty()) {
        LOGDEB("SearchDataClauseDist: [" << s << "]resolved to null query\n");
        if (!pbterm.empty()) 
            m_reason = string("Resolved to null query. Problem term : [" + pbterm + string("]"));
        else
            m_reason = string("Resolved to null query. Term too long ? : [" + m_text + string("]"));
        return true;
    }

    *qp = *pqueries.begin();
    if (m_weight != 1.0) {
        *qp = Xapian::Query(Xapian::Query::OP_SCALE_WEIGHT, *qp, m_weight);
    }
    return true;
}

} // Namespace Rcl
