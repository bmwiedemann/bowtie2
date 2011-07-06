/*
 *  aligner_cache.h
 */

#ifndef ALIGNER_CACHE_H_
#define ALIGNER_CACHE_H_

/**
 * CACHEING
 *
 * By caching the results of some alignment sub-problems, we hope to
 * enable a "fast path" for read alignment whereby answers are mostly
 * looked up rather than calculated from scratch.  This is particularly
 * effective when the input is sorted or otherwise grouped in a way
 * that brings together reads with (at least some) seed sequences in
 * common.
 *
 * But the cache is also where results are held, regardless of whether
 * the results are maintained & re-used across reads.
 *
 * The cache consists of two linked potions:
 *
 * 1. A multimap from seed strings (i.e. read substrings) to reference strings
 *    that are within some edit distance (roughly speaking).  This is the "seed
 *    multimap".
 *
 *    Key:   Read substring (2-bit-per-base encoded + length)
 *    Value: Set of reference substrings (i.e. keys into the suffix
 *           array multimap).
 *
 * 2. A multimap from reference strings to the corresponding elements of the
 *    suffix array.  Elements are filled in with reference-offset info as it's
 *    calculated.  This is the "suffix array multimap"
 *
 *    Key:   Reference substring (2-bit-per-base encoded + length)
 *    Value: (a) top from BWT, (b) length of range, (c) offset of first
 *           range element in 
 *
 * For both multimaps, we use a combo Red-Black tree and EList.  The payload in
 * the Red-Black tree nodes points to a range in the EList.
 */

#include <iostream>
#include "ds.h"
#include "read.h"
#include "threading.h"
#include "mem_ids.h"

#define CACHE_PAGE_SZ (16 * 1024)

typedef PListSlice<uint32_t, CACHE_PAGE_SZ> TSlice;

/**
 * Key for the query multimap: the read substring and its length.
 */
struct QKey {

	/**
	 * Initialize invalid QKey.
	 */
	QKey() { reset(); }

	/**
	 * Initialize QKey from DNA string.
	 */
	QKey(const BTDnaString& s ASSERT_ONLY(, BTDnaString& tmp)) {
		init(s ASSERT_ONLY(, tmp));
	}
	
	/**
	 * Initialize QKey from DNA string.  Rightmost character is placed in the
	 * least significant bitpair.
	 */
	bool init(
		const BTDnaString& s
		ASSERT_ONLY(, BTDnaString& tmp))
	{
		seq = 0;
		len = (uint32_t)s.length();
		ASSERT_ONLY(tmp.clear());
		if(len > 32) {
			len = 0xffffffff;
			return false; // wasn't cacheable
		} else {
			// Rightmost char of 's' goes in the least significant bitpair
			for(size_t i = 0; i < 32 && i < s.length(); i++) {
				int c = (int)s.get(i);
				assert_range(0, 4, c);
				if(c == 4) {
					len = 0xffffffff;
					return false;
				}
				seq = (seq << 2) | s.get(i);
			}
			ASSERT_ONLY(toString(tmp));
			assert(sstr_eq(tmp, s));
			return true; // was cacheable
		}
	}
	
	/**
	 * Convert this key to a DNA string.
	 */
	void toString(BTDnaString& s) {
		s.resize(len);
		uint64_t sq = seq;
		for(int i = (len)-1; i >= 0; i--) {
			s.set(sq & 3, i);
			sq >>= 2;
		}
	}
	
	/**
	 * Return true iff the read substring is cacheable.
	 */
	bool cacheable() const { return len != 0xffffffff; }
	
	/**
	 * Reset to uninitialized state.
	 */
	void reset() { seq = 0; len = 0xffffffff; }

	/**
	 * True -> my key is less than the given key.
	 */
	bool operator<(const QKey& o) const {
		return seq < o.seq || (seq == o.seq && len < o.len);
	}

	/**
	 * True -> my key is greater than the given key.
	 */
	bool operator>(const QKey& o) const {
		return !(*this < o || *this == o);
	}

	/**
	 * True -> my key is equal to the given key.
	 */
	bool operator==(const QKey& o) const {
		return seq == o.seq && len == o.len;
	}


	/**
	 * True -> my key is not equal to the given key.
	 */
	bool operator!=(const QKey& o) const {
		return !(*this == o);
	}
	
	/**
	 * Check that this is a valid, initialized QKey.
	 */
	bool repOk() const {
		return len != 0xffffffff;
	}

	uint64_t seq; // sequence
	uint32_t len; // length of sequence
};

class AlignmentCache;

/**
 * Payload for the query multimap: a range of elements in the reference
 * string list.
 */
class QVal {

public:

	QVal() { reset(); }

	/**
	 * Return the offset of the first reference substring in the qlist.
	 */
	uint32_t offset() const { return i_; }

	/**
	 * Return the number of reference substrings associated with a read
	 * substring.
	 */
	uint32_t numRanges() const {
		assert(valid());
		return rangen_;
	}

	/**
	 * Return the number of elements associated with all associated
	 * reference substrings.
	 */
	uint32_t numElts() const {
		assert(valid());
		return eltn_;
	}
	
	/**
	 * Return true iff the read substring is not associated with any
	 * reference substrings.
	 */
	bool empty() const {
		assert(valid());
		return numRanges() == 0;
	}

	/**
	 * Return true iff the QVal is valid.
	 */
	bool valid() const { return rangen_ != 0xffffffff; }
	
	/**
	 * Reset to invalid state.
	 */
	void reset() {
		i_ = 0; rangen_ = eltn_ = 0xffffffff;
	}
	
	/**
	 * Initialize Qval.
	 */
	void init(uint32_t i, uint32_t ranges, uint32_t elts) {
		i_ = i; rangen_ = ranges; eltn_ = elts;
	}
	
	/**
	 * Tally another range with given number of elements.
	 */
	void addRange(uint32_t numElts) {
		rangen_++;
		eltn_ += numElts;
	}
	
	/**
	 * Check that this QVal is internally consistent and consistent
	 * with the contents of the given cache.
	 */
	bool repOk(const AlignmentCache& ac) const;

protected:

	uint32_t i_;      // idx of first elt in qlist
	uint32_t rangen_; // # ranges (= # associated reference substrings)
	uint32_t eltn_;   // # elements (total)
};

/**
 * Key for the suffix array multimap: the reference substring and its
 * length.  Same as QKey so I typedef it.
 */
typedef QKey SAKey;

/**
 * Payload for the suffix array multimap: (a) the top element of the
 * range in BWT, (b) the offset of the first elt in the salist, (c)
 * length of the range.
 */
struct SAVal {

	SAVal() : top(), i(), len(0xffffffff) { }

	/**
	 * Return true iff the SAVal is valid.
	 */
	bool valid() { return len != 0xffffffff; }

	/**
	 * Check that this SAVal is internally consistent and consistent
	 * with the contents of the given cache.
	 */
	bool repOk(const AlignmentCache& ac) const;
	
	/**
	 * Initialize the SAVal.
	 */
	void init(uint32_t t, uint32_t ii, uint32_t ln) {
		top = t;
		i = ii;
		len = ln;
	}

	uint32_t top; // top in BWT
	uint32_t i;   // idx of first elt in salist
	uint32_t len; // length of range
};

/**
 * One data structure that encapsulates all of the cached information
 * associated with a particular reference substring.  This is useful
 * for summarizing what info should be added to the cache for a partial
 * alignment.
 */
class SATuple {

public:

	SATuple() { reset(); };

	SATuple(SAKey k, uint32_t t, TSlice o) {
		init(k, t, o);
	}
	
	void init(SAKey k, uint32_t t, TSlice o) {
		key = k; top = t; offs = o;
	}

	/**
	 * Initialize this SATuple from a subrange of the SATuple 'src'.
	 */
	void init(const SATuple& src, size_t first, size_t last) {
		key = src.key;
		top = src.top + (uint32_t)first;
		offs.init(src.offs, first, last);
	}
	
	/**
	 * Check that this SATuple is internally consistent and that its
	 * PListSlice is consistent with its backing PList.
	 */
	bool repOk() const {
		assert(offs.repOk());
		return true;
	}

	/**
	 * Randomly narrow down a list of SATuples such that the result has no more
	 * than 'maxrows' rows total.  Could involve splitting some ranges into
	 * pieces.  Return the result in dst.
	 */
	template<typename T>
	static bool randomNarrow(
		const T& src,      // input list of SATuples
		T& dst,            // output list of SATuples
		RandomSource& rnd, // pseudo-random generator
		size_t maxrows)    // max # rows to keep
	{
		// Add up the total number of rows
		size_t totrows = 0;
		for(size_t i = 0; i < src.size(); i++) {
			totrows += src[i].offs.size();
		}
		//std::cerr << "randomNarrow: maxrows=" << maxrows
		//          << ", totrows=" << totrows << std::endl;
		if(totrows <= maxrows) {
			return false;
		}
		size_t totrowsSampled = 0;
		uint32_t off = (uint32_t)(rnd.nextU32() % totrows);
		//std::cerr << "randomNarrow: picked offset " << off << std::endl;
		bool on = false;
		bool done = false;
		// Go around twice, since the 
		totrows = 0;
		for(int twice = 0; twice < 2; twice++) {
			for(size_t i = 0; i < src.size(); i++) {
				assert(src[i].repOk());
				if(!on) {
					// Do we start sampling in this range?
					on = (off < totrows + src[i].offs.size());
					if(on) {
						// Grab the appropriate portion of this range
						assert_geq(off, totrows);
						dst.expand();
						size_t first = off - totrows;
						size_t last = first + maxrows;
						if(last > src[i].offs.size()) {
							last = src[i].offs.size();
						}
						assert_gt(last, first);
						dst.back().init(src[i], first, last);
						totrowsSampled += (last-first);
						assert(dst.back().repOk());
					}
				} else {
					// This range is either in the middle or at the end of
					// the random sample.
					assert_lt(totrowsSampled, maxrows);
					dst.expand();
					size_t first = 0;
					size_t last = maxrows - totrowsSampled;
					if(last > src[i].offs.size()) {
						last = src[i].offs.size();
					}
					assert_gt(last, first);
					dst.back().init(src[i], first, last);
					totrowsSampled += (last-first);
					assert(dst.back().repOk());
				}
				if(totrowsSampled == maxrows) {
					done = true;
					break;
				}
				totrows += src[i].offs.size();
			}
			if(done) break;
			// Must have already encountered first range we're sampling
			// from
			assert(on);
		}
		// Destination must be non-empty can can't have more than 1+
		// the number of elements in the source.  1+ because the
		// sampled range could "wrap around" and touch the same source
		// range twice.
		assert(!dst.empty());
		assert_leq(dst.size(), src.size()+1);
		return true;
	}

	void reset() { top = 0xffffffff; offs.reset(); }

	// bot/length of SA range equals offs.size()
	SAKey    key;  // sequence key
	uint32_t top;  // top in BWT index
	TSlice   offs; // offsets
};

/**
 * Encapsulate the data structures and routines that constitute a
 * particular cache, i.e., a particular stratum of the cache system,
 * which might comprise many strata.
 *
 * Each thread has a "current-read" AlignmentCache which is used to
 * build and store subproblem results as alignment is performed.  When
 * we're finished with a read, we might copy the cached results for
 * that read (and perhaps a bundle of other recently-aligned reads) to
 * a higher-level "across-read" cache.  Higher-level caches may or may
 * not be shared among threads.
 *
 * A cache consists chiefly of two multimaps, each implemented as a
 * Red-Black tree map backed by an EList.  A 'version' counter is
 * incremented every time the cache is cleared.
 */
class AlignmentCache {

	typedef RedBlackNode<QKey,  QVal>  QNode;
	typedef RedBlackNode<SAKey, SAVal> SANode;

	typedef PList<SAKey, CACHE_PAGE_SZ> TQList;
	typedef PList<uint32_t, CACHE_PAGE_SZ> TSAList;

public:

	AlignmentCache(
		uint64_t bytes,
		bool shared) :
		pool_(bytes, CACHE_PAGE_SZ, CA_CAT),
		qmap_(CACHE_PAGE_SZ, CA_CAT),
		qlist_(CA_CAT),
		samap_(CACHE_PAGE_SZ, CA_CAT),
		salist_(CA_CAT),
		shared_(shared),
		version_(0)
	{
		MUTEX_INIT(lock_);
	}

	/**
	 * Returns a pointer to a corresponding QVal if there are one or
	 * more ranges corresponding to sequence 'k' in the cache.  Returns
	 * NULL otherwise.
	 */
	inline QVal* query(const QKey& k, bool getLock = true) {
		ThreadSafe ts(lockPtr(), shared_ && getLock);
		QNode *n = qmap_.lookup(k);
		if(n != NULL) {
			assert(n->payload.repOk(*this));
			// Return a pointer to the payload
			return &n->payload;
		}
		return NULL;
	}

	/**
	 * Given a QKey, populates an EList of SATuples with all of the
	 * corresponding reference substring information.
	 */
	template<int S>
	void queryEx(
		const QKey& k,
		EList<SATuple, S>& satups,
		bool getLock = true)
	{
		ThreadSafe ts(lockPtr(), shared_ && getLock);
		QVal *n = query(k, getLock);
		if(n != NULL) queryQval(*n, satups, false);
	}


	/**
	 * Given a QVal, populate the given EList of SATuples with records
	 * describing all of the cached information about the QVal's
	 * reference substrings.
	 */
	template <int S>
	void queryQval(
		const QVal& qv,
		EList<SATuple, S>& satups,
		bool getLock = true)
	{
		ThreadSafe ts(lockPtr(), shared_ && getLock);
		assert(qv.repOk(*this));
		const size_t refi = qv.offset();
		const size_t reff = refi + qv.numRanges();
		// For each reference sequence sufficiently similar to the
		// query sequence in the QKey.
		for(size_t i = refi; i < reff; i++) {
			// Get corresponding SAKey, containing similar reference
			// sequence & length
			SAKey sak = qlist_.get(i);
			// Shouldn't have identical keys in qlist_
			assert(i == refi || qlist_.get(i) != qlist_.get(i-1));
			// Get corresponding SANode
			SANode *n = samap_.lookup(sak);
			assert(n != NULL);
			const SAVal& sav = n->payload;
			assert(sav.repOk(*this));
			satups.expand();
			satups.back().init(sak, sav.top, TSlice(salist_, sav.i, sav.len));
#ifndef NDEBUG
			// Shouldn't add consecutive identical entries too satups
			if(i > refi) {
				const SATuple b1 = satups.back();
				const SATuple b2 = satups[satups.size()-2];
				assert(b1.key != b2.key || b1.top != b2.top || b1.offs != b2.offs);
			}
#endif
		}
	}

	/**
	 * Return true iff the cache has no entries in it.
	 */
	bool empty() const {
		bool ret = qmap_.empty();
		assert(!ret || qlist_.empty());
		assert(!ret || samap_.empty());
		assert(!ret || salist_.empty());
		return ret;
	}
	
	/**
	 * Copy the query key ('qk') and all associated QVals, SAKeys and
	 * SAVals from the given cache to this cache.  Return true iff the
	 * copy was succesful.  False is returned if memory was exhausted
	 * before the copy could complete.
	 *
	 * TODO: If the copy if aborted in the middle due to memory
	 * exhaustion, remove the partial addition.
	 */
	bool copy(
		const QKey& qk,
		const QVal& qv,
		AlignmentCache& c,
		bool getLock = true)
	{
		ThreadSafe ts(lockPtr(), shared_ && getLock);
		assert(qv.repOk(c));
		assert(qk.cacheable());
		// Try to add a new node; added will be false if we already
		// have qk in this cache.
		bool added = false;
		QNode *n = qmap_.add(pool(), qk, &added);
		if(!added) {
			// Key was already present at destination.
			// TODO: perhaps merge the offsets
			return true;
		}
		assert(n != NULL);
		assert(n->key.repOk());
		// Set the new QVal's i and len
		n->payload.init((uint32_t)qlist_.size(), qv.numRanges(), qv.numElts());
		// Add the ref seqs to this cache's qlist
		const size_t reff = qv.offset() + qv.numRanges();
		for(size_t i = qv.offset(); i < reff; i++) {
			SAKey sak = c.qlist_.get(i);
			if(!qlist_.add(pool(), sak)) {
				// Pool memory exhausted
				assert(qlist_.back().repOk());
				return false;
			}
			SANode *srcSaNode = c.samap_.lookup(sak);
			assert(srcSaNode != NULL);
			assert(srcSaNode->payload.repOk(c));
			SANode *dstSaNode = samap_.add(pool(), sak, &added);
			if(!added) {
				// SAKey already in this cache's samap
				// TODO: possibly merge offsets
				continue;
			}
			if(dstSaNode == NULL) {
				// Pool memory exhausted
				return false;
			}
			uint32_t srci = srcSaNode->payload.i;
			uint32_t top = srcSaNode->payload.top;
			uint32_t len = srcSaNode->payload.len;
			dstSaNode->payload.init(top, (uint32_t)salist_.size(), len);
			// 
			for(size_t j = 0; j < len; j++) {
				if(!salist_.add(pool(), c.salist_.get(srci+j))) {
					// Pool memory exhausted
					return false;
				}
			}
			assert(dstSaNode->payload.repOk(*this));
		}
		// Success
		return true;
	}

	/**
	 * Copy the query key ('qk') and all associated QVals, SAKeys and
	 * SAVals from the given cache to this cache.  Return true iff we
	 * had to clear the cache in order to complete the copy.
	 */
	bool clearCopy(
		const QKey& qk,
		const QVal& qv,
		AlignmentCache& c,
		bool getLock = true)
	{
		ThreadSafe ts(lockPtr(), shared_ && getLock);
		if(!copy(qk, qv, c, false)) {
			// Clear the whole cache
			clear();
			assert(empty());
			// Try again
			if(!copy(qk, qv, c, false)) {
				std::cerr << "Warning: A key couldn't fit in an empty cache.  Try increasing the cache size." << std::endl;
			}
			return true;
		}
		return false;
	}

	/**
	 * Add a new query key ('qk'), usually a 2-bit encoded substring of
	 * the read) as the key in a new Red-Black node in the qmap and
	 * return a pointer to the node's QVal.
	 *
	 * The expectation is that the caller is about to set about finding
	 * associated reference substrings, and that there will be future
	 * calls to addOnTheFly to add associations to reference substrings
	 * found.
	 */
	QVal* add(
		const QKey& qk,
		bool *added,
		bool getLock = true)
	{
		ThreadSafe ts(lockPtr(), shared_ && getLock);
		assert(qk.cacheable());
		QNode *n = qmap_.add(pool(), qk, added);
		return (n != NULL ? &n->payload : NULL);
	}

	/**
	 * Add a new association between a read sequnce ('seq') and a
	 * reference sequence ('')
	 */
	bool addOnTheFly(
		QVal& qv,         // qval that points to the range of reference substrings
		const SAKey& sak, // the key holding the reference substring
		uint32_t topf,    // top range elt in BWT index
		uint32_t botf,    // bottom range elt in BWT index
		bool getLock = true);

	/**
	 * Clear the cache, i.e. turn it over.  All HitGens referring to
	 * ranges in this cache will become invalid and the corresponding
	 * reads will have to be re-aligned.
	 */
	void clear(bool getLock = true) {
		ThreadSafe ts(lockPtr(), shared_ && getLock);
		pool_.clear();
		qmap_.clear();
		qlist_.clear();
		samap_.clear();
		salist_.clear();
		version_++;
	}

	/**
	 * Return the number of keys in the query multimap.
	 */
	size_t qNumKeys() const { return qmap_.size(); }

	/**
	 * Return the number of keys in the suffix array multimap.
	 */
	size_t saNumKeys() const { return samap_.size(); }

	/**
	 * Return the number of elements in the reference substring list.
	 */
	size_t qSize() const { return qlist_.size(); }

	/**
	 * Return the number of elements in the SA range list.
	 */
	size_t saSize() const { return salist_.size(); }

	/**
	 * Return the pool.
	 */
	Pool& pool() { return pool_; }
	
	/**
	 * Return the lock object.
	 */
	MUTEX_T& lock() { return lock_; }

	/**
	 * Return a const pointer to the lock object.  This allows us to
	 * write const member functions that grab the lock.
	 */
	MUTEX_T* lockPtr() const {
		return const_cast<MUTEX_T*>(&lock_);
	}
	
	/**
	 * Return true iff this cache is shared among threads.
	 */
	bool shared() const { return shared_; }
	
	/**
	 * Return the current "version" of the cache, i.e. the total number
	 * of times it has turned over since its creation.
	 */
	uint32_t version() const { return version_; }

protected:

	Pool                   pool_;   // dispenses memory pages
	RedBlack<QKey, QVal>   qmap_;   // map from query substrings to reference substrings
	TQList                 qlist_;  // list of reference substrings
	RedBlack<SAKey, SAVal> samap_;  // map from reference substrings to SA ranges
	TSAList                salist_; // list of SA ranges
	
	bool     shared_;  // true -> this cache is global
	MUTEX_T  lock_;    // lock to grab during writes if this cache is shared
	uint32_t version_; // cache version
};

/**
 * Interface used to query and update a pair of caches: one thread-
 * local and unsynchronized, another shared and synchronized.  One or
 * both can be NULL.
 */
class AlignmentCacheIface {

public:

	AlignmentCacheIface(
		AlignmentCache *current,
		AlignmentCache *local,
		AlignmentCache *shared) :
		qk_(),
		qv_(NULL),
		cacheable_(false),
		rangen_(0),
		eltsn_(0),
		current_(current),
		local_(local),
		shared_(shared)
	{
		assert(current_ != NULL);
	}

	/**
	 * Query the relevant set of caches, looking for a QVal to go with
	 * the provided QKey.  If the QVal is found in a cache other than
	 * the current-read cache, it is copied into the current-read cache
	 * first and the QVal pointer for the current-read cache is
	 * returned.  This function never returns a pointer from any cache
	 * other than the current-read cache.  If the QVal could not be
	 * found in any cache OR if the QVal was found in a cache other
	 * than the current-read cache but could not be copied into the
	 * current-read cache, NULL is returned.
	 */
	QVal* queryCopy(const QKey& qk, bool getLock = true) {
		AlignmentCache* caches[3] = { current_, local_, shared_ };
		for(int i = 0; i < 3; i++) {
			if(caches[i] == NULL) continue;
			QVal* qv = caches[i]->query(qk, getLock);
			if(qv != NULL) {
				if(i == 0) return qv;
				if(!current_->copy(qk, *qv, *caches[i], getLock)) {
					// Exhausted memory in the current cache while
					// attempting to copy in the qk
					return NULL;
				}
				QVal* curqv = current_->query(qk, getLock);
				assert(curqv != NULL);
				return curqv;
			}
		}
		return NULL;
	}

	/**
	 * Query the relevant set of caches, looking for a QVal to go with
	 * the provided QKey.  If a QVal is found and which is non-NULL,
	 * *which is set to 0 if the qval was found in the current-read
	 * cache, 1 if it was found in the local across-read cache, and 2
	 * if it was found in the shared across-read cache.
	 */
	inline QVal* query(
		const QKey& qk,
		AlignmentCache** which,
		bool getLock = true)
	{
		AlignmentCache* caches[3] = { current_, local_, shared_ };
		for(int i = 0; i < 3; i++) {
			if(caches[i] == NULL) continue;
			QVal* qv = caches[i]->query(qk, getLock);
			if(qv != NULL) {
				if(which != NULL) *which = caches[i];
				return qv;
			}
		}
		return NULL;
	}

	/**
	 * This function is called whenever we start to align a new read or
	 * read substring.  We make key for it and store the key in qk_.
	 * If the sequence is uncacheable, we don't actually add it to the
	 * map but the corresponding reference substrings are still added
	 * to the qlist_.
	 *
	 * Returns:
	 *  -1 if out of memory
	 *  0 if key was found in cache
	 *  1 if key was not found in cache (and there's enough memory to
	 *    add a new key)
	 */
	int beginAlign(
		const BTDnaString& seq,
		const BTString& qual,
		QVal& qv,              // out: filled in if we find it in the cache
		bool getLock = true)
	{
		assert(repOk());
		qk_.init(seq ASSERT_ONLY(, tmpdnastr_));
		if(qk_.cacheable() && (qv_ = current_->query(qk_, getLock)) != NULL) {
			// qv_ holds the answer
			assert(qv_->valid());
			qv = *qv_;
			resetRead();
			return 1; // found in cache
		} else if(qk_.cacheable()) {
			// Make a QNode for this key and possibly add the QNode to the
			// Red-Black map; but if 'seq' isn't cacheable, just create the
			// QNode (without adding it to the map).
			qv_ = current_->add(qk_, &cacheable_, getLock);
		} else {
			qv_ = &qvbuf_;
		}
		if(qv_ == NULL) {
			resetRead();
 			return -1; // Not in memory
		}
		qv_->reset();
		return 0; // Need to search for it
	}
	ASSERT_ONLY(BTDnaString tmpdnastr_);
	
	/**
	 * Called when is finished aligning a read (and so is finished
	 * adding associated reference strings).  Returns a copy of the
	 * final QVal object and resets the alignment state of the
	 * current-read cache.
	 *
	 * Also, if the alignment is cacheable, it commits it to the next
	 * cache up in the cache hierarchy.
	 */
	QVal finishAlign(bool getLock = true) {
		if(!qv_->valid()) {
			qv_->init(0, 0, 0);
		}
		// Copy this pointer because we're about to reset the qv_ field
		// to NULL
		QVal* qv = qv_;
		// Commit the contents of the current-read cache to the next
		// cache up in the hierarchy.
		// If qk is cacheable, then it must be in the cache
		if(qk_.cacheable()) {
			AlignmentCache* caches[3] = { current_, local_, shared_ };
			ASSERT_ONLY(AlignmentCache* which);
			ASSERT_ONLY(QVal* qv2 = query(qk_, &which, true));
			assert(qv2 == qv);
			assert(which == current_);
			for(int i = 1; i < 3; i++) {
				if(caches[i] != NULL) {
					// Copy this key/value pair to the to the higher
					// level cache and, if its memory is exhausted,
					// clear the cache and try again.
					caches[i]->clearCopy(qk_, *qv_, *current_, getLock);
					break;
				}
			}
		}
		// Reset the state in this iface in preparation for the next
		// alignment.
		resetRead();
		assert(repOk());
		return *qv;
	}

	/**
	 * A call to this member indicates that the caller has finished
	 * with the last read (if any) and is ready to work on the next.
	 * This gives the cache a chance to reset some of its state if
	 * necessary.
	 */
	void nextRead() {
		current_->clear();
		resetRead();
		assert(!aligning());
	}
	
	/**
	 * Return true iff we're in the middle of aligning a sequence.
	 */
	bool aligning() const {
		return qv_ != NULL;
	}
	
	/**
	 * Clears both the local and shared caches.
	 */
	void clear() {
		if(current_ != NULL) current_->clear();
		if(local_   != NULL) local_->clear();
		if(shared_  != NULL) shared_->clear();
	}
	
	/**
	 * Add an alignment to the running list of alignments being
	 * compiled for the current read in the local cache.
	 */
	bool addOnTheFly(
		const BTDnaString& rfseq, // reference sequence close to read seq
		uint32_t topf,            // top in BWT index
		uint32_t botf,            // bot in BWT index
		bool getLock = true)      // true -> lock is not held by caller
	{
		
		assert(aligning());
		assert(repOk());
		ASSERT_ONLY(BTDnaString tmp);
		SAKey sak(rfseq ASSERT_ONLY(, tmp));
		assert(sak.cacheable());
		if(current_->addOnTheFly((*qv_), sak, topf, botf, getLock)) {
			rangen_++;
			eltsn_ += (botf-topf);
			return true;
		}
		return false;
	}

	/**
	 * Given a QKey, populates an EList of SATuples with all of the
	 * corresponding reference substring information.
	 */
	template<int S>
	void queryEx(
		const QKey& k,
		EList<SATuple, S>& satups,
		bool getLock = true)
	{
		current_->queryEx(k, satups, getLock);
	}


	/**
	 * Given a QVal, populate the given EList of SATuples with records
	 * describing all of the cached information about the QVal's
	 * reference substrings.
	 */
	template<int S>
	void queryQval(
		const QVal& qv,
		EList<SATuple, S>& satups,
		bool getLock = true)
	{
		current_->queryQval(qv, satups, getLock);
	}

	/**
	 * Return a pointer to the current-read cache object.
	 */
	const AlignmentCache* currentCache() const { return current_; }
	
	size_t curNumRanges() const { return rangen_; }
	size_t curNumElts()   const { return eltsn_;  }
	
	/**
	 * Check that AlignmentCacheIface is internally consistent.
	 */
	bool repOk() const {
		assert(current_ != NULL);
		assert_geq(eltsn_, rangen_);
		if(qv_ == NULL) {
			assert_eq(0, rangen_);
			assert_eq(0, eltsn_);
		}
		return true;
	}
	
	/**
	 * Return the alignment cache for the current read.
	 */
	const AlignmentCache& current() {
		return *current_;
	}

protected:

	/**
	 * Reset fields encoding info about the in-process read.
	 */
	void resetRead() {
		cacheable_ = false;
		rangen_ = eltsn_ = 0;
		qv_ = NULL;
	}

	QKey qk_;  // key representation for current read substring
	QVal *qv_; // pointer to value representation for current read substring
	QVal qvbuf_; // buffer for when key is uncacheable but we need a qv
	bool cacheable_; // true iff the read substring currently being aligned is cacheable
	
	size_t rangen_; // number of ranges since last alignment job began
	size_t eltsn_;  // number of elements since last alignment job began

	AlignmentCache *current_; // cache dedicated to the current read
	AlignmentCache *local_;   // local, unsynchronized cache
	AlignmentCache *shared_;  // shared, synchronized cache
};

#endif /*ALIGNER_CACHE_H_*/
