/** @file
 * @brief PostList class implementing Query::OP_OR
 */
/* Copyright 2017 Olly Betts
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <config.h>

#include "orpostlist.h"

#include "andmaybepostlist.h"
#include "andpostlist.h"
#include "min_non_zero.h"
#include "postlisttree.h"

#include <algorithm>

using namespace std;

template<typename T>
static void
estimate_or_assuming_indep(double a, double b, double n, T& res)
{
    if (rare(n == 0.0)) {
	res = 0;
    } else {
	res = static_cast<T>(a + b - (a * b / n) + 0.5);
    }
}

OrPostList::OrPostList(PostList* left, PostList* right,
		       PostListTree* pltree_, Xapian::doccount db_size)
    : l(left), r(right), pltree(pltree_)
{
    estimate_or_assuming_indep(l->get_termfreq(),
			       r->get_termfreq(),
			       db_size,
			       termfreq);
}

PostList*
OrPostList::decay_to_and(Xapian::docid did,
			 double w_min,
			 bool* valid_ptr)
{
    l = new AndPostList(l, r, l_max, r_max, pltree, termfreq);
    r = NULL;
    PostList* result;
    if (valid_ptr) {
	result = l->check(did, w_min, *valid_ptr);
    } else {
	result = l->skip_to(did, w_min);
    }
    if (!result) {
	result = l;
	l = NULL;
    }
    pltree->force_recalc();
    return result;
}

PostList*
OrPostList::decay_to_andmaybe(PostList* left,
			      PostList* right,
			      Xapian::docid did,
			      double w_min,
			      bool* valid_ptr)
{
    if (l != left) swap(l_max, r_max);
    l = new AndMaybePostList(left, right, l_max, r_max, pltree);
    r = NULL;
    PostList* result;
    if (valid_ptr) {
	result = l->check(did, w_min, *valid_ptr);
    } else {
	result = l->skip_to(did, w_min);
    }
    if (!result) {
	result = l;
	l = NULL;
    }
    pltree->force_recalc();
    return result;
}

Xapian::docid
OrPostList::get_docid() const
{
    // Handle l_did or r_did being zero correctly (which means the last call on
    // that side was a check() which came back !valid).
    return min_non_zero(l_did, r_did);
}

double
OrPostList::get_weight(Xapian::termcount doclen,
		       Xapian::termcount unique_terms,
		       Xapian::termcount wdfdocmax) const
{
    if (r_did == 0 || l_did < r_did)
	return l->get_weight(doclen, unique_terms, wdfdocmax);
    if (l_did == 0 || l_did > r_did)
	return r->get_weight(doclen, unique_terms, wdfdocmax);
    return l->get_weight(doclen, unique_terms, wdfdocmax) +
	   r->get_weight(doclen, unique_terms, wdfdocmax);
}

double
OrPostList::recalc_maxweight()
{
    l_max = l->recalc_maxweight();
    r_max = r->recalc_maxweight();
    return l_max + r_max;
}

PostList*
OrPostList::next(double w_min)
{
    if (w_min > l_max) {
	if (w_min > r_max) {
	    // Work out the smallest docid which the AND could match at.
	    Xapian::docid did;
	    if (l_did == r_did || r_did == 0) {
		did = l_did + 1;
	    } else if (l_did == 0) {
		did = r_did + 1;
	    } else {
		// The OR last matched at min(l_did, r_did), so the AND could
		// match at the max().
		did = max(l_did, r_did);
	    }
	    return decay_to_and(did, w_min);
	}
	// Work out the smallest docid which r AND_MAYBE l could match at.
	Xapian::docid did;
	if (r_did == 0) {
	    did = l_did + 1;
	} else if (l_did - 1 >= r_did - 1) {
	    did = r_did + 1;
	} else {
	    // l_did and r_did both non zero and l_did < r_did.
	    did = r_did;
	}
	return decay_to_andmaybe(r, l, did, w_min);
    }
    if (w_min > r_max) {
	// Work out the smallest docid which l AND_MAYBE r could match at.
	Xapian::docid did;
	if (l_did == 0) {
	    did = r_did + 1;
	} else if (r_did - 1 >= l_did - 1) {
	    did = l_did + 1;
	} else {
	    // l_did and r_did both non zero and r_did < l_did.
	    did = l_did;
	}
	return decay_to_andmaybe(l, r, did, w_min);
    }

    // We always advance_l if l_did is 0, and similarly for advance_r.
    bool advance_l = (l_did <= r_did);
    bool advance_r = (l_did >= r_did);

    if (advance_l) {
	PostList* result = l->next(w_min - r_max);
	if (result) {
	    delete l;
	    l = result;
	}
    }

    if (advance_r) {
	PostList* result = r->next(w_min - l_max);
	if (result) {
	    delete r;
	    r = result;
	}
    }

    if (advance_l) {
	if (l->at_end()) {
	    PostList* result = r;
	    r = NULL;
	    pltree->force_recalc();
	    return result;
	}
    }

    if (advance_r) {
	if (r->at_end()) {
	    PostList* result = l;
	    l = NULL;
	    pltree->force_recalc();
	    return result;
	}
    }

    if (advance_l) {
	l_did = l->get_docid();
    }

    if (advance_r) {
	r_did = r->get_docid();
    }

    return NULL;
}

PostList*
OrPostList::skip_to(Xapian::docid did, double w_min)
{
    // We always advance_l if l_did is 0, and similarly for advance_r.
    bool advance_l = (did > l_did);
    bool advance_r = (did > r_did);
    if (!advance_l && !advance_r)
	return NULL;

    if (w_min > l_max) {
	if (w_min > r_max)
	    return decay_to_and(did, w_min);
	return decay_to_andmaybe(r, l, did, w_min);
    }
    if (w_min > r_max) {
	return decay_to_andmaybe(l, r, did, w_min);
    }

    if (advance_l) {
	PostList* result = l->skip_to(did, w_min - r_max);
	if (result) {
	    delete l;
	    l = result;
	}
    }

    if (advance_r) {
	PostList* result = r->skip_to(did, w_min - l_max);
	if (result) {
	    delete r;
	    r = result;
	}
    }

    if (advance_l) {
	if (l->at_end()) {
	    PostList* result = r;
	    r = NULL;
	    pltree->force_recalc();
	    return result;
	}
    }

    if (advance_r) {
	if (r->at_end()) {
	    PostList* result = l;
	    l = NULL;
	    pltree->force_recalc();
	    return result;
	}
    }

    if (advance_l) {
	l_did = l->get_docid();
    }

    if (advance_r) {
	r_did = r->get_docid();
    }

    return NULL;
}

PostList*
OrPostList::check(Xapian::docid did, double w_min, bool& valid)
{
    bool advance_l = (did > l_did);
    bool advance_r = (did > r_did);
    if (!advance_l && !advance_r) {
	// A call to check() which steps back isn't valid, so if we get here
	// then did should be equal to at least one of l_did or r_did.
	Assert(did == l_did || did == r_did);
	valid = true;
	return NULL;
    }

    if (w_min > l_max) {
	valid = true;
	if (w_min > r_max)
	    return decay_to_and(did, w_min, &valid);
	return decay_to_andmaybe(r, l, did, w_min, &valid);
    }
    if (w_min > r_max) {
	valid = true;
	return decay_to_andmaybe(l, r, did, w_min, &valid);
    }

    if (advance_l) {
	bool l_valid;
	PostList* result = l->check(did, w_min - r_max, l_valid);
	if (result) {
	    Assert(l_valid);
	    delete l;
	    l = result;
	} else if (!l_valid) {
	    l_did = 0;
	    advance_l = false;
	}
    }

    if (advance_r) {
	bool r_valid;
	PostList* result = r->check(did, w_min - l_max, r_valid);
	if (result) {
	    Assert(r_valid);
	    delete r;
	    r = result;
	} else if (!r_valid) {
	    r_did = 0;
	    advance_r = false;
	}
    }

    if (advance_l) {
	if (l->at_end()) {
	    PostList* result = r;
	    r = NULL;
	    pltree->force_recalc();
	    valid = true;
	    return result;
	}
    }

    if (advance_r) {
	if (r->at_end()) {
	    PostList* result = l;
	    l = NULL;
	    pltree->force_recalc();
	    valid = true;
	    return result;
	}
    }

    if (advance_l) {
	l_did = l->get_docid();
    }

    if (advance_r) {
	r_did = r->get_docid();
    }

    valid = (l_did == did || r_did == did) || (l_did != 0 && r_did != 0);

    return NULL;
}

bool
OrPostList::at_end() const
{
    // We never need to return true here - if one child reaches at_end(), we
    // prune to leave the other, and if both children reach at_end() together,
    // we prune to leave one of them which will then indicate at_end() for us.
    return false;
}

std::string
OrPostList::get_description() const
{
    string desc = "OrPostList(";
    desc += l->get_description();
    desc += ", ";
    desc += r->get_description();
    desc += ')';
    return desc;
}

Xapian::termcount
OrPostList::get_wdf() const
{
    if (r_did == 0 || l_did < r_did)
	return l->get_wdf();
    if (l_did == 0 || l_did > r_did)
	return r->get_wdf();
    return l->get_wdf() + r->get_wdf();
}

Xapian::termcount
OrPostList::count_matching_subqs() const
{
    if (r_did == 0 || l_did < r_did)
	return l->count_matching_subqs();
    if (l_did == 0 || l_did > r_did)
	return r->count_matching_subqs();
    return l->count_matching_subqs() + r->count_matching_subqs();
}

void
OrPostList::gather_position_lists(OrPositionList* orposlist)
{
    if (l_did - 1 <= r_did - 1)
	l->gather_position_lists(orposlist);
    if (l_did - 1 >= r_did - 1)
	r->gather_position_lists(orposlist);
}
