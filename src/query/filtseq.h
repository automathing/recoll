/* Copyright (C) 2004 J.F.Dockes
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
#ifndef _FILTSEQ_H_INCLUDED_
#define _FILTSEQ_H_INCLUDED_
#include "autoconfig.h"

#include <vector>
#include <string>
#include <memory>

#include "docseq.h"

class RclConfig;

/** 
 * A filtered sequence is created from another one by selecting entries
 * according to the given criteria.
 * Note that this class can only filter on mime type (or rclcatg) at the moment,
 * and is only used for history. Normal query filtering is performed by adding a
 * clause to the Xapian query.
 */
class DocSeqFiltered : public DocSeqModifier {
public:
    DocSeqFiltered(RclConfig *conf, std::shared_ptr<DocSequence> iseq, DocSeqFiltSpec &filtspec);
    virtual ~DocSeqFiltered() {}
    DocSeqFiltered(const DocSeqFiltered&) = delete;
    DocSeqFiltered& operator=(const DocSeqFiltered&) = delete;
    virtual bool canFilter() override {return true;}
    virtual bool setFiltSpec(const DocSeqFiltSpec &filtspec) override;
    virtual bool getDoc(int num, Rcl::Doc &doc, std::string *sh = nullptr) override;
    virtual int getResCnt() override {return m_seq->getResCnt();}
private:
    RclConfig     *m_config;    
    DocSeqFiltSpec m_spec;
    std::vector<int> m_dbindices;
};

#endif /* _FILTSEQ_H_INCLUDED_ */
