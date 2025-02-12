/* Copyright (C) 2004-2023 J.F.Dockes
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

#include <stdio.h>
#include <cstdlib>
#include <errno.h>

#include <string>

#include "unacpp.h"
#include "unac.h"
#include "log.h"
#include "utf8iter.h"

using namespace std;

bool unacmaybefold(const string &in, string &out, UnacOp what)
{
    char *cout = 0;
    size_t out_len;
    int status = -1;
    
    switch (what) {
    case UNACOP_UNAC:
        status = unac_u8string(in.c_str(), in.length(), &cout, &out_len);
        break;
    case UNACOP_UNACFOLD:
        status = unacfold_u8string(in.c_str(), in.length(), &cout, &out_len);
        break;
    case UNACOP_FOLD:
        status = fold_u8string(in.c_str(), in.length(), &cout, &out_len);
        break;
    }

    if (status < 0) {
        if (cout)
            free(cout);
        out = string("unac_string failed, errno : ") + std::to_string(errno);
        return false;
    }
    out.assign(cout, out_len);
    if (cout)
        free(cout);
    return true;
}


std::string unactolower(const std::string& in)
{
    std::string out;
    unacmaybefold(in, out, UNACOP_FOLD);
    return out;
}

// Functions to determine upper-case or accented status could be implemented
// hugely more efficiently inside the unac c code, but there only used for
// testing user-entered terms, so we don't really care.
bool unaciscapital(const string& in)
{
    LOGDEB2("unaciscapital: [" << in << "]\n");
    if (in.empty())
        return false;
    Utf8Iter it(in);
    string shorter;
    it.appendchartostring(shorter);

    string lower;
    if (!unacmaybefold(shorter, lower, UNACOP_FOLD)) {
        LOGINFO("unaciscapital: unac/fold failed for [" << in << "]\n");
        return false;
    } 
    Utf8Iter it1(lower);
    if (*it != *it1)
        return true;
    else
        return false;
}

// Check if input contains upper case characters. We used to case-fold
// the input and look for a difference, but lowercasing and
// casefolding are actually not exactly the same, for example german
// sharp s folds to ss but lowercases to itself, and greek final sigma
// folds to sigma. So an input containing one of these characters
// would wrongly detected as containing upper case. We now handle a
// few special cases explicitly, by folding them before performing
// the lowercasing. There are actually quite a few other cases of
// lowercase being transformed by casefolding, check Unicode
// CaseFolding.txt for occurrences of SMALL. One more step towards
// ditching everything and using icu...
bool unachasuppercase(const string& _in)
{
    LOGDEB("unachasuppercase: in [" << _in << "]\n");
    if (_in.empty())
        return false;
    string in;
    Utf8Iter it(_in);
    for (; !it.eof(); it++) {
        if (*it == 0xdf) {
            // s sharp -> ss
            in += 's';
            in += 's';
        } else if (*it == 0x3c2) {
            // final sigma -> sigma
            in.append("\xcf\x83");
        } else {
            it.appendchartostring(in);
        }
    }
    LOGDEB("unachasuppercase: folded: [" << in << "]\n");
    
    string lower;
    if (!unacmaybefold(in, lower, UNACOP_FOLD)) {
        LOGINFO("unachasuppercase: unac/fold failed for [" << in << "]\n");
        return false;
    } 
    LOGDEB("unachasuppercase: lower [" << lower << "]\n");
    if (lower != in)
        return true;
    else
        return false;
}

bool unachasaccents(const string& in)
{
    LOGDEB("unachasaccents: in [" << in << "]\n");
    if (in.empty())
        return false;

    string noac;
    if (!unacmaybefold(in, noac, UNACOP_UNAC)) {
        LOGINFO("unachasaccents: unac/unac failed for ["  << (in) << "]\n" );
        return false;
    } 
    LOGDEB("unachasaccents: noac [" << noac << "]\n");
    if (noac != in)
        return true;
    else
        return false;
}
