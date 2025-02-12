/* Copyright (C) 2014-2019 J.F.Dockes
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

#include "syngroups.h"

#include <unordered_map>
#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>

#include "log.h"
#include "smallut.h"
#include "pathut.h"


using namespace std;

// Note that we are storing each term twice. I don't think that the
// size could possibly be a serious issue, but if it was, we could
// reduce the storage a bit by storing (short hash)-> vector<int>
// correspondances in the direct map, and then checking all the
// resulting groups for the input word.
//
// As it is, a word can only index one group (the last it is found
// in). It can be part of several groups though (appear in
// expansions). I really don't know what we should do with multiple
// groups anyway
class SynGroups::Internal {
public:
    Internal() {}
    void setpath(const string& fn) {
        path = path_canon(fn);
        path_fileprops(path, &st);
    }
    bool samefile(const string& fn) {
        string p1 = path_canon(fn);
        if (path != p1) {
            return false;
        }
        struct PathStat st1;
        if (path_fileprops(p1, &st1) != 0) {
            return false;
        }
        return st.pst_mtime == st1.pst_mtime && st.pst_size == st1.pst_size;
    }

    void clear() {
        ok = false;
        terms.clear();
        groups.clear();
        multiwords.clear();
        multiwords_maxlen = 0;
        path.clear();
        st.pst_mtime = 0;
        st.pst_size = 0;
    }
    
    bool ok{false};
    // Term to group num 
    std::unordered_map<string, size_t> terms;
    // Group num to group
    vector<vector<string> > groups;

    // Aux: set of multiword synonyms used for generating multiword
    // terms while indexing
    std::set<std::string> multiwords;
    size_t multiwords_maxlen{0};
    
    std::string path;
    struct PathStat st;
};

bool SynGroups::ok() const
{
    return m && m->ok;
}

SynGroups::~SynGroups()
{
    delete m;
}

SynGroups::SynGroups()
    : m(new Internal)
{
}

bool SynGroups::setfile(const string& fn)
{
    LOGDEB("SynGroups::setfile(" << fn << ")\n");
    if (!m) {
        return false;
    }

    if (fn.empty()) {
        m->clear();
        return true;
    }

    if (m->samefile(fn)) {
        LOGDEB("SynGroups::setfile: unchanged: " << fn << endl);
        return true;
    }
    LOGDEB("SynGroups::setfile: parsing file " << fn << endl);
    
    ifstream input;
    input.open(fn.c_str(), ios::in);
    if (!input.is_open()) {
        LOGSYSERR("SynGroups:setfile", "open", fn);
        return false;
    }        

    string cline;
    bool appending = false;
    string line;
    bool eof = false;
    int lnum = 0;
    m->clear();
    for (;;) {
        cline.clear();
        getline(input, cline);
        if (!input.good()) {
            if (input.bad()) {
                LOGERR("Syngroup::setfile(" << fn << "):Parse: input.bad()\n");
                return false;
            }
            // Must be eof ? But maybe we have a partial line which
            // must be processed. This happens if the last line before
            // eof ends with a backslash, or there is no final \n
            eof = true;
        }
        lnum++;

        {
            string::size_type pos = cline.find_last_not_of("\n\r");
            if (pos == string::npos) {
                cline.clear();
            } else if (pos != cline.length()-1) {
                cline.erase(pos+1);
            }
        }

        if (appending)
            line += cline;
        else
            line = cline;

        // Note that we trim whitespace before checking for backslash-eol
        // This avoids invisible whitespace problems.
        trimstring(line);
        if (line.empty() || line.at(0) == '#') {
            if (eof)
                break;
            continue;
        }
        if (line[line.length() - 1] == '\\') {
            line.erase(line.length() - 1);
            appending = true;
            continue;
        }
        appending = false;

        vector<string> words;
        if (!stringToStrings(line, words)) {
            LOGERR("SynGroups:setfile: " << fn << ": bad line " << lnum << ": " << line << "\n");
            continue;
        }

        if (words.empty())
            continue;
        if (words.size() == 1) {
            LOGERR("Syngroup::setfile(" << fn << "):single term group at line " << lnum << " ??\n");
            continue;
        }
        for (auto& word : words) {
            if (word.find_first_of(" \t") != std::string::npos) {
                vector<string> v;
                stringToTokens(word, v);
                word = tokensToString(v);
            }
            m->terms[word] = m->groups.size();
        }
        m->groups.push_back(words);
        LOGDEB1("SynGroups::setfile: group: [" << stringsToString(m->groups.back()) << "]\n");
    }

    for (const auto& group : m->groups) {
        for (const auto& term : group) {
            // Whitespace was already normalized to single 0x20
            if (term.find(' ') != std::string::npos) {
                size_t cnt = std::count(term.begin(), term.end(), ' ') + 1;
                m->multiwords.insert(term);
                if (m->multiwords_maxlen < cnt) {
                    m->multiwords_maxlen = cnt;
                }
            }
        }
    }
    LOGDEB("SynGroups::setfile: got " << m->groups.size() << " distinct terms. "
           "Multiwords: " << stringsToString(m->multiwords) <<"\n");
    m->ok = true;
    m->setpath(fn);
    return true;
}

vector<string> SynGroups::getgroup(const string& term) const
{
    vector<string> ret;
    if (!ok())
        return ret;

    const auto it1 = m->terms.find(term);
    if (it1 == m->terms.end()) {
        LOGDEB0("SynGroups::getgroup: [" << term << "] not found in map\n");
        return ret;
    }

    size_t idx = it1->second;
    if (idx >= m->groups.size()) {
        LOGERR("SynGroups::getgroup: line index higher than line count !\n");
        return ret;
    }
    LOGDEB0("SynGroups::getgroup: result: " << stringsToString(m->groups[idx]) << '\n');
    return m->groups[idx];
}

const std::set<std::string>& SynGroups::getmultiwords() const
{
    return m->multiwords;
}

size_t SynGroups::getmultiwordsmaxlength() const
{
    return m->multiwords_maxlen;
}

const std::string& SynGroups::getpath() const
{
    static string empty;
    return m ? m->path : empty;
}
