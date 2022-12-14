#include "masstree.h"
#include "Epoche.cpp"

using namespace MASS;

/* Global pool pointer */
extern PMEMobjpool *pop;
extern uint64_t pool_uuid;
extern size_t pool_size;

namespace masstree {

static constexpr uint64_t CACHE_LINE_SIZE = 64;

static inline void fence() {
    asm volatile("" ::: "memory");
}

static inline void mfence() {
    asm volatile("mfence":::"memory");
}

static inline void clflush(char *data, int len, bool fence)
{
    volatile char *ptr = (char *)((unsigned long)data &~(CACHE_LINE_SIZE-1));
    if (fence)
        mfence();
    for(; ptr<data+len; ptr+=CACHE_LINE_SIZE){
#ifdef CLFLUSH
        asm volatile("clflush %0" : "+m" (*(volatile char *)ptr));
#elif CLFLUSH_OPT
        asm volatile(".byte 0x66; clflush %0" : "+m" (*(volatile char *)(ptr)));
#elif CLWB
        asm volatile(".byte 0x66; xsaveopt %0" : "+m" (*(volatile char *)(ptr)));
#endif
    }
    if (fence)
        mfence();
}

static inline void movnt64(uint64_t *dest, uint64_t const &src) {
    mfence();
    _mm_stream_si64((long long int *)dest, *(long long int *)&src);
    mfence();
}

static inline void prefetch_(const void *ptr)
{
    typedef struct { char x[CACHE_LINE_SIZE]; } cacheline_t;
    asm volatile("prefetcht0 %0" : : "m" (*(const cacheline_t *)ptr));
}

static inline void *ptr_from_off(uint64_t offset)
{
    PMEMoid oid = {pool_uuid, offset};
    return pmemobj_direct(oid);
}

masstree::masstree() {
    if (root_ == 0) {
        leafnode *init_root = new leafnode(0);
        clflush((char *)init_root, sizeof(leafnode), true);
        root_ = pmemobj_oid(init_root).off;
        clflush((char *)&root_, sizeof(uint64_t), true);
        remote_start_addr = (uint64_t)pop;
        clflush((char *)&remote_start_addr, sizeof(uint64_t), true);
    }
}

masstree::masstree(void *new_root) {
    clflush((char *)new_root, sizeof(leafnode), true);
    root_ = pmemobj_oid(new_root).off;
    clflush((char *)&root_, sizeof(uint64_t), true);
}

void *masstree::operator new(size_t size) {
    PMEMoid ret;
    if (pmemobj_zalloc(pop, &ret, size, 0)) {
        fprintf(stderr, "%s: pmemobj_zalloc failed\n", __func__);
        assert(0);
    }
    return pmemobj_direct(ret);
}

void masstree::operator delete(void *addr) {
    PMEMoid treeRoot = pmemobj_oid(ptr_from_off(((masstree *)addr)->root_));
    pmemobj_free(&treeRoot);
    PMEMoid indexRoot = pmemobj_oid(addr);
    pmemobj_free(&indexRoot);
}

leafnode::leafnode(uint32_t level) : permutation(permuter::make_empty()) {
    level_ = level;
    next = OID_NULL.off;
    leftmost_ptr = OID_NULL.off;
}

leafnode::leafnode(void *left, uint64_t key, void *right, uint32_t level = 1) : permutation(permuter::make_empty()) {
    level_ = level;
    next = OID_NULL.off;
    leftmost_ptr = pmemobj_oid(left).off;
    entry[0].key = key;
    entry[0].value = pmemobj_oid(right).off;

    permutation = permuter::make_sorted(1);
}

void *leafnode::operator new(size_t size) {
    PMEMoid ret;
    if (pmemobj_zalloc(pop, &ret, size, 0)) {
        fprintf(stderr, "%s: pmemobj_zalloc failed\n", __func__);
        assert(0);
    }
    return pmemobj_direct(ret);
}

void leafnode::operator delete(void *addr) {
    PMEMoid leaf_oid = pmemobj_oid(addr);
    pmemobj_free(&leaf_oid);
}

bool leafnode::isLocked(uint64_t version) const {
    return ((version & 0b10) == 0b10);
}

void leafnode::writeLock() {
    uint64_t version;
    version = typeVersionLockObsolete.load();
    int needRestart = UNLOCKED;
    upgradeToWriteLockOrRestart(version, needRestart);
}

void leafnode::writeLockOrRestart(int &needRestart) {
    uint64_t version;
    needRestart = UNLOCKED;
    version = readLockOrRestart(needRestart);
    if (needRestart) return;

    upgradeToWriteLockOrRestart(version, needRestart);
}

bool leafnode::tryLock(int &needRestart) {
    uint64_t version;
    needRestart = UNLOCKED;
    version = readLockOrRestart(needRestart);
    if (needRestart) return false;

    upgradeToWriteLockOrRestart(version, needRestart);
    if (needRestart) return false;

    return true;
}

void leafnode::upgradeToWriteLockOrRestart(uint64_t &version, int &needRestart) {
    if (typeVersionLockObsolete.compare_exchange_strong(version, version + 0b10)) {
        version = version + 0b10;
    } else {
        needRestart = LOCKED;
    }
}

void leafnode::writeUnlock(bool isOverWrite) {
    if (isOverWrite)
        typeVersionLockObsolete.fetch_add(0b10);
    else
        typeVersionLockObsolete.fetch_sub(0b10);
}

uint64_t leafnode::readLockOrRestart(int &needRestart) const {
    uint64_t version;
    version = typeVersionLockObsolete.load();
    needRestart = UNLOCKED;

    if (isLocked(version))
        needRestart = LOCKED;
    else if (isObsolete(version))
        needRestart = OBSOLETE;

    return version;
}

bool leafnode::isObsolete(uint64_t version) {
    return (version & 1) == 1;
}

void leafnode::checkOrRestart(uint64_t startRead, int &needRestart) const {
    readUnlockOrRestart(startRead, needRestart);
}

void leafnode::readUnlockOrRestart(uint64_t startRead, int &needRestart) const {
    needRestart = (int)(startRead != typeVersionLockObsolete.load());
}

int leafnode::compare_key(const uint64_t a, const uint64_t b)
{
    if (a == b)
        return 0;
    else
        return a < b ? -1 : 1;
}

key_indexed_position leafnode::key_lower_bound_by(uint64_t key)
{
    permuter perm = permutation;
    int l = 0, r = perm.size();
    while (l < r) {
        int m = (l + r) >> 1;
        int mp = perm[m];
        int cmp = compare_key(key, entry[mp].key);
        if (cmp < 0)
            r = m;
        else if (cmp == 0)
            return key_indexed_position(m, mp);
        else
            l = m + 1;
    }
    return key_indexed_position(l, -1);
}

key_indexed_position leafnode::key_lower_bound(uint64_t key)
{
    permuter perm = permutation;
    int l = 0, r = perm.size();
    while (l < r) {
        int m = (l + r) >> 1;
        int mp = perm[m];
        int cmp = compare_key(key, entry[mp].key);
        if (cmp < 0)
            r = m;
        else if (cmp == 0)
            return key_indexed_position(m, mp);
        else
            l = m + 1;
    }

    return (l-1 < 0 ? key_indexed_position(l-1, -1) : key_indexed_position(l-1, perm[l-1]));
}

leafnode *leafnode::advance_to_key(const uint64_t& key)
{
    const leafnode *n = this;

    leafnode *snapshot_n;
    if ((snapshot_n = reinterpret_cast<leafnode *>(ptr_from_off(n->next))) 
            && compare_key(key, snapshot_n->highest) >= 0) {
        n = snapshot_n;
    }

    return const_cast<leafnode *> (n);
}

leafnode *leafnode::advance_to_key(masstree *t, const uint64_t& key, uint64_t &raddr, int &buf_idx,
        uint32_t lkey, struct ibv_qp *qp, leafnode *recv_buf_ptr, uint32_t rkey, struct ibv_cq *cq)
{
    const leafnode *n = this;

    leafnode *snapshot_n;
    if (n->next != 0) {
        buf_idx == 1 ? buf_idx-- : buf_idx++;
        post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp,
                (char *)&recv_buf_ptr[buf_idx], t->raddr_from_off(n->next), rkey, cq);
        snapshot_n = reinterpret_cast<leafnode *> (&recv_buf_ptr[buf_idx]);
        if (compare_key(key, snapshot_n->highest) >= 0) {
            raddr = t->raddr_from_off(n->next);
            n = snapshot_n;
        } else {
            buf_idx == 1 ? buf_idx-- : buf_idx++;
        }
    }

    return const_cast<leafnode *> (n);
}

permuter leafnode::permute()
{
    return permutation;
}

void leafnode::prefetch() const
{
    for (int i = 64; i < std::min((16 * LEAF_WIDTH) + 1, 4 * 64); i += 64)
        prefetch_((const char *) this + i);
}

leafvalue *masstree::make_leaf(char *key, size_t key_len, uint64_t value, uint32_t val_len)
{
    leafvalue *lv = NULL;
    size_t len = (key_len % sizeof(uint64_t)) == 0 ? key_len : (((key_len) / sizeof(uint64_t)) + 1) * sizeof(uint64_t);

    if (value != 0) {
        PMEMoid ret;
        if (pmemobj_zalloc(pop, &ret, sizeof(leafvalue) + len + sizeof(uint64_t), 0)) {
            fprintf(stderr, "%s: pmemobj_zalloc failed\n", __func__);
            assert(0);
        }
        lv = reinterpret_cast<leafvalue *> (pmemobj_direct(ret));
    } else {
        lv = (leafvalue *) calloc(1, sizeof(leafvalue) + len + sizeof(uint64_t));
    }

    lv->value = value;
    lv->val_len = val_len;
    lv->key_len = key_len;          // key_len or len??
    memcpy(lv->fkey, key, key_len);

    for (uint64_t i = 0; i < (len / sizeof(uint64_t)); i++)
        lv->fkey[i] = __builtin_bswap64(lv->fkey[i]);

    if (value != 0)
        clflush((char *) lv, sizeof(leafvalue) + len + sizeof(uint64_t), true);
    return lv;
}

leafvalue *leafnode::smallest_leaf(size_t key_len, uint64_t value)
{
    size_t len = (key_len % sizeof(uint64_t)) == 0 ? key_len : (((key_len) / sizeof(uint64_t)) + 1) * sizeof(uint64_t);

    leafvalue *lv = (leafvalue *) malloc(sizeof(leafvalue) + len);
    memset(lv, 0, sizeof(leafvalue) + len);

    lv->value = value;
    lv->key_len = key_len;          // key_len or len??

    for (uint64_t i = 0; i < (len / sizeof(uint64_t)); i++)
        lv->fkey[i] = 0ULL;

    if (value != 0)
        clflush((char *) lv, sizeof(leafvalue) + len, true);
    return lv;
}

void leafnode::make_new_layer(leafnode *l, key_indexed_position &kx_, leafvalue *olv,
        leafvalue *nlv, uint32_t depth)
{
    int kcmp = compare_key(olv->fkey[depth], nlv->fkey[depth]);

    leafnode *twig_head = l;
    leafnode *twig_tail = l;
    while (kcmp == 0) {
        leafnode *nl = new leafnode(0);
        nl->assign_initialize_for_layer(0, olv->fkey[depth]);
        if (twig_head != l)
            twig_tail->entry[0].value = pmemobj_oid(nl).off;
        else
            twig_head = nl;
        nl->permutation = permuter::make_sorted(1);
        twig_tail = nl;
        depth++;
        kcmp = compare_key(olv->fkey[depth], nlv->fkey[depth]);
    }

    leafnode *nl = new leafnode(0);
    nl->assign_initialize(0, kcmp < 0 ? olv->fkey[depth] : nlv->fkey[depth], kcmp < 0 ? SET_LV(pmemobj_oid(olv).off) : SET_LV(pmemobj_oid(nlv).off));
    nl->assign_initialize(1, kcmp < 0 ? nlv->fkey[depth] : olv->fkey[depth], kcmp < 0 ? SET_LV(pmemobj_oid(nlv).off) : SET_LV(pmemobj_oid(olv).off));

    nl->permutation = permuter::make_sorted(2);

    fence();
    if (twig_tail != l)
        twig_tail->entry[0].value = pmemobj_oid(nl).off;
    twig_tail = nl;
    if (twig_head != l) {
        leafnode *iter = twig_head;
        mfence();
        for ( ; iter != twig_tail && iter != NULL; iter = reinterpret_cast <leafnode *>(ptr_from_off(iter->entry[0].value))) {
            clflush((char *)iter, sizeof(leafnode), false);
        }
        clflush((char *)twig_tail, sizeof(leafnode), false);
        mfence();

        l->entry[kx_.p].value = pmemobj_oid(twig_head).off;
        clflush((char *)l->entry_addr(kx_.p) + 8, sizeof(uintptr_t), true);
    } else {
        clflush((char *)nl, sizeof(leafnode), true);

        l->entry[kx_.p].value = pmemobj_oid(nl).off;
        clflush((char *)l->entry_addr(kx_.p) + 8, sizeof(uintptr_t), true);
    }
}

void leafnode::check_for_recovery(masstree *t, leafnode *left, leafnode *right, void *root, uint32_t depth, leafvalue *lv)
{
    permuter perm = left->permute();

    for (int i = perm.size() - 1; i >= 0; i--) {
        if (left->key(perm[i]) >= right->highest) {
            perm.remove_to_back(i);
        } else {
            break;
        }
    }

    if (left->permutation.size() != perm.size()) {
        left->permutation = perm.value();
        clflush((char *)&left->permutation, sizeof(permuter), true);
    }

    if (depth > 0) {
        key_indexed_position pkx_;
        leafnode *p = correct_layer_root(root, lv, depth, pkx_);
        if (p->value(pkx_.p) == pmemobj_oid(left).off) {
            leafnode *new_root = new leafnode(left, right->highest, right, left->level() + 1);
            clflush((char *) new_root, sizeof(leafnode), true);
            p->entry[pkx_.p].value = pmemobj_oid(new_root).off;
            clflush((char *) &p->entry[pkx_.p].value, sizeof(uintptr_t), true);
            p->writeUnlock(false);

            right->writeUnlock(false);
            left->writeUnlock(false);
        } else {
            root = p;
            t->split(ptr_from_off(p->entry[pkx_.p].value), root, depth, lv, right->highest, right, left->level() + 1, left, false);
        }
    } else {
        if (t->root() == pmemobj_oid(left).off) {
            leafnode *new_root = new leafnode(left, right->highest, right, left->level() + 1);
            clflush((char *) new_root, sizeof(leafnode), true);
            t->setNewRoot(new_root);

            right->writeUnlock(false);
            left->writeUnlock(false);
        } else {
            t->split(NULL, NULL, 0, NULL, right->highest, right, left->level() + 1, left, false);
        }
    }
}

void masstree::put(uint64_t key, void *value)// ThreadInfo &threadEpocheInfo)
{
//    EpocheGuard epocheGuard(threadEpocheInfo);
    key_indexed_position kx_;
    leafnode *next = NULL, *p = NULL;
    uint64_t snapshot_v = 0;

    int needRestart;
    uint64_t v;

from_root:
    p = reinterpret_cast<leafnode *> (ptr_from_off(this->root_));
    while (p->level() != 0) {
inter_retry:
        next = p->advance_to_key(key);
        if (next != p) {
            // check for recovery
            if (p->tryLock(needRestart)) {
                if (next->tryLock(needRestart))
                    p->check_for_recovery(this, p, next, NULL, 0, NULL);
                else
                    p->writeUnlock(false);
            }
            p = next;
            goto inter_retry;
        }

        v = p->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED)
                goto inter_retry;
            else
                goto from_root;
        }

        p->prefetch();
        fence();

        kx_ = p->key_lower_bound(key);

        if (kx_.i >= 0)
            snapshot_v = p->value(kx_.p);
        else
            snapshot_v = p->leftmost();

        p->checkOrRestart(v, needRestart);
        if (needRestart)
            goto inter_retry;
        else
            p = reinterpret_cast<leafnode *> (ptr_from_off(snapshot_v));
    }

    leafnode *l = reinterpret_cast<leafnode *> (p);
leaf_retry:
    next = l->advance_to_key(key);
    if (next != l) {
        //check for recovery
        if (l->tryLock(needRestart)) {
            if (next->tryLock(needRestart))
                l->check_for_recovery(this, l, next, NULL, 0, NULL);
            else
                l->writeUnlock(false);
        }

        l = next;
        goto leaf_retry;
    }

    l->writeLockOrRestart(needRestart);
    if (needRestart) {
        if (needRestart == LOCKED)
            goto leaf_retry;
        else // needRestart == OBSOLETE
            goto from_root;
    }

    next = l->advance_to_key(key);
    if (next != l) {
        l->writeUnlock(false);
        l = next;
        goto leaf_retry;
    }

    l->prefetch();
    fence();

    kx_ = l->key_lower_bound_by(key);
    if (kx_.p >= 0 && l->key(kx_.p) == key) {
        l->assign_value(kx_.p, value);
        l->writeUnlock(false);
    } else {
        if (!(l->leaf_insert(this, NULL, 0, NULL, key, value, kx_))) {
            put(key, value);// threadEpocheInfo);
        }
    }
}

void masstree::put(char *key, size_t key_len, uint64_t value, uint32_t val_len, log_block *curr_log_block) //ThreadInfo &threadEpocheInfo)
{
//    EpocheGuard epocheGuard(threadEpocheInfo);
    void *root = NULL;
    key_indexed_position kx_;
    uint32_t depth = 0;
    leafnode *next = NULL, *p = NULL;
    leafvalue *lv = make_leaf(key, key_len, value, val_len);
    uint64_t snapshot_v = 0;

    int needRestart;
    uint64_t v;

restart:
    root = ptr_from_off(this->root_);
    depth = 0;
    p = reinterpret_cast<leafnode *> (root);

from_root:
    while (p->level() != 0) {
inter_retry:
        next = p->advance_to_key(lv->fkey[depth]);
        if (next != p) {
            // check for recovery
            if (p->tryLock(needRestart)) {
                if (next->tryLock(needRestart))
                    p->check_for_recovery(this, p, next, root, depth, lv);
                else
                    p->writeUnlock(false);
            }
            p = next;
            goto inter_retry;
        }

        v = p->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED)
                goto inter_retry;
            else
                goto restart;
        }

        p->prefetch();
        fence();

        kx_ = p->key_lower_bound(lv->fkey[depth]);

        if (kx_.i >= 0)
            snapshot_v = p->value(kx_.p);
        else
            snapshot_v = p->leftmost();

        p->checkOrRestart(v, needRestart);
        if (needRestart)
            goto inter_retry;
        else
            p = reinterpret_cast<leafnode *> (ptr_from_off(snapshot_v));
    }

    leafnode *l = reinterpret_cast<leafnode *> (p);
leaf_retry:
    next = l->advance_to_key(lv->fkey[depth]);
    if (next != l) {
        //check for recovery
        if (l->tryLock(needRestart)) {
            if (next->tryLock(needRestart))
                l->check_for_recovery(this, l, next, root, depth, lv);
            else
                l->writeUnlock(false);
        }
        l = next;
        goto leaf_retry;
    }

    l->writeLockOrRestart(needRestart);
    if (needRestart) {
        if (needRestart == LOCKED)
            goto leaf_retry;
        else // needRestart == OBSOLETE
            goto restart;
    }

    next = l->advance_to_key(lv->fkey[depth]);
    if (next != l) {
        l->writeUnlock(false);
        l = next;
        goto leaf_retry;
    }

    l->prefetch();
    fence();

    kx_ = l->key_lower_bound_by(lv->fkey[depth]);
    if (kx_.p >= 0) {
        // i)   If there is additional layer, retry B+tree traversing from the next layer
        if (!IS_LV(l->value(kx_.p))) {
            p = reinterpret_cast<leafnode *> (ptr_from_off(l->value(kx_.p)));
            root = l;
            depth++;
            l->writeUnlock(false);
            goto from_root;
        // ii)  Atomically update value for the matching key
        } else if (IS_LV(l->value(kx_.p)) && ((leafvalue *)ptr_from_off((uint64_t)(LV_PTR(l->value(kx_.p)))))->key_len == lv->key_len &&
                memcmp(lv->fkey, ((leafvalue *)ptr_from_off((uint64_t)(LV_PTR(l->value(kx_.p)))))->fkey, lv->key_len) == 0) {
            snapshot_v = ((leafvalue *)ptr_from_off((uint64_t)(LV_PTR(l->value(kx_.p)))))->value;
            ((leafvalue *)ptr_from_off((uint64_t)(LV_PTR(l->value(kx_.p)))))->value = value;
            clflush((char *)&((leafvalue *)ptr_from_off((uint64_t)(LV_PTR(l->value(kx_.p)))))->value, sizeof(uint64_t), true);
            l->writeUnlock(true);

            PMEMoid invalid_lv = pmemobj_oid(lv);
            pmemobj_free(&invalid_lv);

            oplogs *oLog = reinterpret_cast<oplogs *>(ptr_from_off(snapshot_v - sizeof(oplogs)));
            log_block *log_block_merged = (log_block *)((uint64_t)oLog - (uint64_t)oLog->offset - LOG_BLOCK_HEADER_LEN);
            uint32_t valid_counter = log_block_merged->valid_counter.fetch_sub(1);
            if (valid_counter == 1) {
                if (log_block_merged != curr_log_block) {
                    reserved_alloc_queue->push((uint64_t)log_block_merged);
                    //PMEMoid invalid_log_block = pmemobj_oid(log_block_merged);
                    //pmemobj_free(&invalid_log_block);
                }
            }

        // iii) Allocate additional layers (B+tree's roots) up to
        //      the number of common prefixes (8bytes unit).
        //      Insert two keys to the leafnode in the last layer
        //      During these processes, this leafnode must be locked
        } else {
            l->make_new_layer(l, kx_, ((leafvalue *)ptr_from_off((uint64_t)(LV_PTR(l->value(kx_.p))))), lv, ++depth);
            l->writeUnlock(false);
        }
    } else {
        if (!(l->leaf_insert(this, root, depth, lv, lv->fkey[depth], SET_LV(pmemobj_oid(lv).off), kx_))) {
            put(key, key_len, value, val_len, curr_log_block);
        }
    }
}

void masstree::del(uint64_t key, ThreadInfo &threadEpocheInfo)
{
    EpocheGuard epocheGuard(threadEpocheInfo);
    void *root = NULL;
    key_indexed_position kx_;
    leafnode *next = NULL;
    uint64_t snapshot_v = 0;

    int needRestart;
    uint64_t v;

restart:
    root = ptr_from_off(this->root_);
    leafnode *p = reinterpret_cast<leafnode *> (root);
    while (p->level() != 0) {
inter_retry:
        next = p->advance_to_key(key);
        if (next != p) {
            // check for recovery
            if (p->tryLock(needRestart)) {
                if (next->tryLock(needRestart)) {
                    p->check_for_recovery(this, p, next, NULL, 0, NULL);
                } else
                    p->writeUnlock(false);
            }
            p = next;
            goto inter_retry;
        }

        v = p->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED)
                goto inter_retry;
            else
                goto restart;
        }

        p->prefetch();
        fence();

        kx_ = p->key_lower_bound(key);

        if (kx_.i >= 0)
            snapshot_v = p->value(kx_.p);
        else
            snapshot_v = p->leftmost();

        p->checkOrRestart(v, needRestart);
        if (needRestart)
            goto inter_retry;
        else
            p = reinterpret_cast<leafnode *> (ptr_from_off(snapshot_v));
    }

    leafnode *l = reinterpret_cast<leafnode *> (p);
leaf_retry:
    next = l->advance_to_key(key);
    if (next != l) {
        // check for recovery
        if (l->tryLock(needRestart)) {
            if (next->tryLock(needRestart)) {
                l->check_for_recovery(this, l, next, NULL, 0, NULL);
            } else
                l->writeUnlock(false);
        }

        l = next;
        goto leaf_retry;
    }

    l->writeLockOrRestart(needRestart);
    if (needRestart) {
        if (needRestart == LOCKED)
            goto leaf_retry;
        else
            goto restart;
    }

    next = l->advance_to_key(key);
    if (next != l) {
        l->writeUnlock(false);
        l = next;
        goto leaf_retry;
    }

    l->prefetch();
    fence();

    kx_ = l->key_lower_bound_by(key);
    if (kx_.p < 0) {
        l->writeUnlock(false);
        return ;
    }

    if (!(l->leaf_delete(this, NULL, 0, NULL, kx_, threadEpocheInfo))) {
        del(key, threadEpocheInfo);
    }
}

void masstree::del(char *key, ThreadInfo &threadEpocheInfo)
{
    EpocheGuard epocheGuard(threadEpocheInfo);
    void *root = NULL;
    key_indexed_position kx_;
    uint32_t depth = 0;
    leafnode *next = NULL;
    uint64_t snapshot_v = 0;

    leafvalue *lv = make_leaf(key, strlen(key), 0, 0);

    int needRestart;
    uint64_t v;

restart:
    depth = 0;
    root = ptr_from_off(this->root_);
    leafnode *p = reinterpret_cast<leafnode *> (root);
from_root:
    while (p->level() != 0) {
inter_retry:
        next = p->advance_to_key(lv->fkey[depth]);
        if (next != p) {
            // check for recovery
            if (p->tryLock(needRestart)) {
                if (next->tryLock(needRestart))
                    p->check_for_recovery(this, p, next, root, depth, lv);
                else
                    p->writeUnlock(false);
            }
            p = next;
            goto inter_retry;
        }

        v = p->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED)
                goto inter_retry;
            else
                goto restart;
        }

        p->prefetch();
        fence();

        kx_ = p->key_lower_bound(lv->fkey[depth]);

        if (kx_.i >= 0)
            snapshot_v = p->value(kx_.p);
        else
            snapshot_v = p->leftmost();

        p->checkOrRestart(v, needRestart);
        if (needRestart)
            goto inter_retry;
        else
            p = reinterpret_cast<leafnode *> (ptr_from_off(snapshot_v));
    }

    leafnode *l = reinterpret_cast<leafnode *> (p);
leaf_retry:
    next = l->advance_to_key(lv->fkey[depth]);
    if (next != l) {
        //check for recovery
        if (l->tryLock(needRestart)) {
            if (next->tryLock(needRestart))
                l->check_for_recovery(this, l, next, root, depth, lv);
            else
                l->writeUnlock(false);
        }

        l = next;
        goto leaf_retry;
    }

    l->writeLockOrRestart(needRestart);
    if (needRestart) {
        if (needRestart == LOCKED)
            goto leaf_retry;
        else
            goto restart;
    }

    next = l->advance_to_key(lv->fkey[depth]);
    if (next != l) {
        l->writeUnlock(false);
        l = next;
        goto leaf_retry;
    }

    l->prefetch();
    fence();

    kx_ = l->key_lower_bound_by(lv->fkey[depth]);
    if (kx_.p >= 0) {
        // i)   If there is additional layer, retry B+tree traversing from the next layer
        if (!IS_LV(l->value(kx_.p))) {
            p = reinterpret_cast<leafnode *> (ptr_from_off(l->value(kx_.p)));
            root = l;
            depth++;
            l->writeUnlock(false);
            goto from_root;
        // ii)  Checking false-positive result and starting to delete it
        } else if (IS_LV(l->value(kx_.p)) && ((leafvalue *)ptr_from_off((uint64_t)LV_PTR(l->value(kx_.p))))->key_len == lv->key_len &&
                memcmp(lv->fkey, ((leafvalue *)ptr_from_off((uint64_t)LV_PTR(l->value(kx_.p))))->fkey, lv->key_len) == 0) {
            if (!(l->leaf_delete(this, root, depth, lv, kx_, threadEpocheInfo))) {
                free(lv);
                del(key, threadEpocheInfo);
            }
        } else {
            l->writeUnlock(false);
            free(lv);
            return ;
        }
    } else {
        l->writeUnlock(false);
        free(lv);
        return ;
    }
}

inline void leafnode::assign_initialize(int p, const uint64_t& key, void *value)
{
    entry[p].key = key;
    entry[p].value = (uint64_t) value;
}

inline void leafnode::assign_initialize(int p, leafnode *x, int xp)
{
    entry[p].key = x->entry[xp].key;
    entry[p].value = x->entry[xp].value;
}

inline void leafnode::assign_initialize_for_layer(int p, const uint64_t& key)
{
    entry[p].key = key;
}

int leafnode::split_into(leafnode *nr, int p, const uint64_t& key, void *value, uint64_t& split_key)
{
    int width = this->permutation.size();
    int mid = width / 2 + 1;

    permuter perml = this->permutation;
    permuter pv = perml.value_from(mid - (p < mid));
    for (int x = mid; x <= width; ++x) {
        if (x == p)
            nr->assign_initialize(x - mid, key, value);
        else {
            nr->assign_initialize(x - mid, this, pv & 15);
            pv >>= 4;
        }
    }

    permuter permr = permuter::make_sorted(width + 1 - mid);
    if (p >= mid)
        permr.remove_to_back(p - mid);
    nr->permutation = permr.value();

    //leafnode::link_split(this, nr);
    nr->highest = nr->entry[0].key;
    nr->next = this->next;
    clflush((char *)nr, sizeof(leafnode), true);
    this->next = pmemobj_oid(nr).off;
    clflush((char *)(&this->next), sizeof(uintptr_t), true);

    split_key = nr->highest;
    return p >= mid ? 1 + (mid == LEAF_WIDTH) : 0;
}

void leafnode::split_into_inter(leafnode *nr, uint64_t& split_key)
{
    int width = this->permutation.size();
    int mid = width / 2 + 1;

    permuter perml = this->permutation;
    permuter pv = perml.value_from(mid);
    for (int x = mid; x < width; ++x) {
        nr->assign_initialize(x - mid, this, pv & 15);
        pv >>= 4;
    }

    permuter permr = permuter::make_sorted(width - mid);
    nr->permutation = permr.value();

    nr->leftmost_ptr = this->entry[perml[mid - 1]].value;
    nr->highest = this->entry[perml[mid - 1]].key;
    nr->next = this->next;
    clflush((char *)nr, sizeof(leafnode), true);
    this->next = pmemobj_oid(nr).off;
    clflush((char *)(&this->next), sizeof(uint64_t), true);

    split_key = nr->highest;
}

void leafnode::assign(int p, const uint64_t& key, void *value)
{
    entry[p].key = key;
    fence();
    entry[p].value = (uint64_t) value;
}

void leafnode::assign_value(int p, void *value)
{
    entry[p].value = (uint64_t) value;
    clflush((char *)&entry[p].value, sizeof(uint64_t), true);
}

void *leafnode::entry_addr(int p)
{
    return &entry[p];
}

void masstree::setNewRoot(void *new_root)
{
    this->root_ = pmemobj_oid(new_root).off;
    clflush((char *)&this->root_, sizeof(uint64_t), true);
}

leafnode *leafnode::correct_layer_root(void *root, leafvalue *lv, uint32_t depth, key_indexed_position &pkx_)
{
    int needRestart;
    leafnode *oldp;
    leafnode *p = reinterpret_cast<leafnode *> (root);

leaf_retry:
    oldp = p->advance_to_key(lv->fkey[depth - 1]);
    if (oldp != p) {
        p = oldp;
        goto leaf_retry;
    }
    
    p->writeLockOrRestart(needRestart);
    if (needRestart)
        goto leaf_retry;
    
    oldp = p->advance_to_key(lv->fkey[depth - 1]);
    if (oldp != p) {
        p->writeUnlock(false);
        p = oldp;
        goto leaf_retry;
    }

    p->prefetch();
    fence();

    pkx_ = p->key_lower_bound_by(lv->fkey[depth - 1]);
    if (pkx_.p < 0) {
        printf("[correct_layer_root] cannot find layer's root\n");
        printf("key = %lu, depth = %u\n", lv->fkey[depth - 1], depth);
        for (int i = 0; i < p->permutation.size(); i++) {
            printf("key[%d] = %lu\n", i, p->entry[p->permutation[i]].key);
        }
        exit(0);
    }

    root = p;
    return p;
}

leafnode *leafnode::search_for_leftsibling(uint64_t *root, uint64_t key, uint32_t level, leafnode *right)
{
    leafnode *p = NULL;
    key_indexed_position kx_;
    leafnode *next = NULL;
    uint64_t snapshot_v = 0;

    int needRestart;
    uint64_t v;

from_root:
    p = reinterpret_cast<leafnode *> (pmemobj_direct({pool_uuid, *root}));
    while (p->level() > level) {
inter_retry:
        next = p->advance_to_key(key);
        if (next != p) {
            p = next;
            goto inter_retry;
        }

        v = p->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED)
                goto inter_retry;
            else
                goto from_root;
        }

        p->prefetch();
        fence();

        kx_ = p->key_lower_bound(key);

        if (kx_.i >= 0)
            snapshot_v = p->value(kx_.p);
        else
            snapshot_v = p->leftmost();

        p->checkOrRestart(v, needRestart);
        if (needRestart)
            goto inter_retry;
        else
            p = reinterpret_cast<leafnode *> (ptr_from_off(snapshot_v));
    }

leaf_retry:
    next = p->advance_to_key(key);
    if (next != p) {
        p = next;
        goto leaf_retry;
    }

    if (p->tryLock(needRestart)) {
        next = p->advance_to_key(key);
        if (next != p) {
            p->writeUnlock(false);
            p = next;
            goto leaf_retry;
        }
    } else {
        if (needRestart == OBSOLETE)
            goto from_root;

        if (p == right)
            return p;
        goto leaf_retry;
    }

    return p;
}

void *leafnode::leaf_insert(masstree *t, void *root, uint32_t depth, leafvalue *lv, uint64_t key, void *value, key_indexed_position &kx_)
{
    bool isOverWrite = false;
    void *ret = NULL;

    // permutation based insert
    if (this->permutation.size() < LEAF_WIDTH) {
        kx_.p = this->permutation.back();
        if (entry[kx_.p].value != 0)
            isOverWrite = true;

        this->assign(kx_.p, key, value);
        clflush((char *)(&this->entry[kx_.p]), sizeof(kv), true);

        permuter cp = this->permutation.value();
        cp.insert_from_back(kx_.i);
        fence();
        this->permutation = cp.value();
        clflush((char *)(&this->permutation), sizeof(permuter), true);

        this->writeUnlock(isOverWrite);
        ret = this;
    } else {
        // overflow
        // leafnode split

        // TODO: Need to add an additional context here checking crash and running recovery
        // mechanism to avoid the duplicate split for the node that was split, but the
        // permutor was not updated to only reflect the half of the entries.
        // * Algorithm sketch:
        // i) compare the high key of right sibling with the highest key of left sibling.
        // ii) If the highest key of left sibling is larger than the high key of right sibling,
        //     1) invalidate the right sibling by atomically changing the next pointer of left
        //        sibling to the next pointer of right sibling. And, continue to the split process.
        //        (current implementation reflects this way, but will be changed to second method)
        //     2) replay the original split process from the third step that removes the half of
        //        the entries from the left sibling. (this would be more reasonable in terms of
        //        reusing the existing split mechanism)
        if (this->next != 0 && this->key(this->permutation[this->permutation.size() - 1]) 
                > ((leafnode *)ptr_from_off(this->next))->highest) {
            this->next = ((leafnode *)ptr_from_off(this->next))->next;
            clflush((char *)&this->next, sizeof(uint64_t), true);
        }

        leafnode *new_sibling = new leafnode(this->level_);
        new_sibling->writeLock();
        uint64_t split_key;
        int split_type = this->split_into(new_sibling, kx_.i, key, value, split_key);

        leafnode *nl = reinterpret_cast<leafnode *> (this);
        leafnode *nr = reinterpret_cast<leafnode *> (new_sibling);

        permuter perml = nl->permutation;
        int width = perml.size();
        perml.set_size(width - nr->permutation.size());

        if (width != LEAF_WIDTH)
            perml.exchange(perml.size(), LEAF_WIDTH - 1);

        nl->permutation = perml.value();
        clflush((char *)(&nl->permutation), sizeof(permuter), true);

        if (depth > 0) {
            key_indexed_position pkx_;
            leafnode *p = correct_layer_root(root, lv, depth, pkx_);
            if (p->value(pkx_.p) == pmemobj_oid(this).off) {
                leafnode *new_root = new leafnode(this, split_key, new_sibling, level_ + 1);
                clflush((char *) new_root, sizeof(leafnode), true);
                p->entry[pkx_.p].value = pmemobj_oid(new_root).off;
                clflush((char *) &p->entry[pkx_.p].value, sizeof(uintptr_t), true);
                p->writeUnlock(false);
            } else {
                root = p;
                t->split(ptr_from_off(p->entry[pkx_.p].value), root, depth, lv, split_key, new_sibling, level_ + 1, NULL, false);
            }
        } else {
            if (t->root() == pmemobj_oid(this).off) {
                leafnode *new_root = new leafnode(this, split_key, new_sibling, level_ + 1);
                clflush((char *) new_root, sizeof(leafnode), true);
                t->setNewRoot(new_root);
            } else {
                t->split(NULL, NULL, 0, NULL, split_key, new_sibling, level_ + 1, NULL, false);
            }
        }

        // permutation base final insertion
        if (split_type == 0) {
            kx_.p = perml.back();
            if (nl->entry[kx_.p].value != 0)
                isOverWrite = true;

            nl->assign(kx_.p, key, value);
            clflush((char *)(&nl->entry[kx_.p]), sizeof(kv), true);

            permuter cp = nl->permutation.value();
            cp.insert_from_back(kx_.i);
            fence();
            nl->permutation = cp.value();
            clflush((char *)(&nl->permutation), sizeof(permuter), true);
            ret = nl;
        } else {
            kx_.i = kx_.p = kx_.i - perml.size();

            permuter cp = nr->permutation.value();
            cp.insert_from_back(kx_.i);
            fence();
            nr->permutation = cp.value();
            clflush((char *)(&nr->permutation), sizeof(permuter), true);
            ret = nr;
        }

        nr->writeUnlock(false);
        nl->writeUnlock(isOverWrite);
    }

    return ret;
}

void *leafnode::leaf_delete(masstree *t, void *root, uint32_t depth, leafvalue *lv, key_indexed_position &kx_, ThreadInfo &threadInfo)
{
    int merge_state;
    void *ret = NULL;

    // permutation based remove
    if (this->permutation.size() > LEAF_THRESHOLD) {
        permuter cp = this->permutation.value();
        cp.remove_to_back(kx_.i);
        fence();
        this->permutation = cp.value();
        clflush((char *)(&this->permutation), sizeof(permuter), true);
        if (lv != NULL) threadInfo.getEpoche().markNodeForDeletion(ptr_from_off(((uint64_t)LV_PTR(this->value(kx_.p)))), threadInfo);
        this->writeUnlock(false);
        ret = this;
    } else {
        // Underflow
        // Merge
        permuter cp;
        leafnode *nl, *nr;
        nr = reinterpret_cast<leafnode *> (this);

        if (depth > 0) {
            key_indexed_position pkx_;
            leafnode *p = correct_layer_root(root, lv, depth, pkx_);
            if (p->value(pkx_.p) == pmemobj_oid(nr).off) {
                cp = nr->permutation.value();
                cp = cp.make_empty();
                fence();
                nr->permutation = cp.value();
                clflush((char *)(&nr->permutation), sizeof(permuter), true);
                p->writeUnlock(false);
                nr->writeUnlock(false);
                return nr;
            } else {
                nl = search_for_leftsibling(&p->entry[pkx_.p].value, nr->highest ? nr->highest - 1 : nr->highest, nr->level_, nr);
                merge_state = t->merge(ptr_from_off(p->entry[pkx_.p].value), reinterpret_cast<void *> (p), depth, lv, nr->highest, nr->level_ + 1, threadInfo);
                if (merge_state == 16) {
                    p = correct_layer_root(root, lv, depth, pkx_);
                    p->entry[pkx_.p].value = pmemobj_oid(nr).off;
                    clflush((char *)&p->entry[pkx_.p].value, sizeof(void *), true);
                    p->writeUnlock(false);
                }
            }
        } else {
            if (t->root() == pmemobj_oid(nr).off) {
                cp = nr->permutation.value();
                cp = cp.make_empty();
                fence();
                nr->permutation = cp.value();
                clflush((char *)(&nr->permutation), sizeof(permuter), true);
                nr->writeUnlock(false);
                return nr;
            } else {
                nl = search_for_leftsibling(t->root_dp(), nr->highest ? nr->highest - 1 : nr->highest, nr->level_, nr);
                merge_state = t->merge(NULL, NULL, 0, NULL, nr->highest, nr->level_ + 1, threadInfo);
                if (merge_state == 16)
                    t->setNewRoot(nr);
            }
        }

        // Final step for node reclamation
        // next pointer is changed, except for leftmost child
        if (merge_state >= 0 && merge_state < 16) {
            nl->next = nr->next;
            clflush((char *)(&nl->next), sizeof(leafnode *), true);
        }

        cp = nr->permutation.value();
        cp = cp.make_empty();
        nr->permutation = cp.value();
        clflush((char *)(&nr->permutation), sizeof(permuter), true);

        if (nl != nr) {
            if (merge_state >= 0 && merge_state < 16) {
                nr->writeUnlockObsolete();
                threadInfo.getEpoche().markNodeForDeletion(nr, threadInfo);
            } else {
                nr->writeUnlock(false);
            }
            nl->writeUnlock(false);
        } else {
            if (merge_state >= 0 && merge_state < 16) {
                nr->writeUnlockObsolete();
                threadInfo.getEpoche().markNodeForDeletion(nr, threadInfo);
            } else {
                nr->writeUnlock(false);
            }
        }
        ret = nr;
    }

    assert(ret != NULL);
    return ret;
}

void *leafnode::inter_insert(masstree *t, void *root, uint32_t depth, leafvalue *lv, 
        uint64_t key, void *value, key_indexed_position &kx_, leafnode *child, bool child_isOverWrite)
{
    bool isOverWrite = false;
    void *ret;

    // permutation based insert
    if (this->permutation.size() < LEAF_WIDTH) {
        kx_.p = this->permutation.back();
        if (entry[kx_.p].value != 0)
            isOverWrite = true;

        this->assign(kx_.p, key, (void *)pmemobj_oid(value).off);
        clflush((char *)(&this->entry[kx_.p]), sizeof(kv), true);

        permuter cp = this->permutation.value();
        cp.insert_from_back(kx_.i);
        fence();
        this->permutation = cp.value();
        clflush((char *)(&this->permutation), sizeof(permuter), true);

        if (child != NULL) {
            ((leafnode *)ptr_from_off(child->next))->writeUnlock(false);
            child->writeUnlock(child_isOverWrite);
        }

        this->writeUnlock(isOverWrite);
        ret = this;
    } else {
        // overflow
        // internode split

        // TODO: Need to add an additional context here checking crash and running recovery
        // mechanism to avoid the duplicate split for the node that was split, but the
        // permutor was not updated to only reflect the half of the entries.
        // * Algorithm sketch:
        // i) compare the high key of right sibling with the highest key of left sibling.
        // ii) If the highest key of left sibling is larger than the high key of right sibling,
        //     1) invalidate the right sibling by atomically changing the next pointer of left
        //        sibling to the next pointer of right sibling. And, continue to the split process.
        //        (current implementation reflects this way, but will be changed to second method)
        //     2) replay the original split process from the third step that removes the half of
        //        the entries from the left sibling. (this would be more reasonable in terms of
        //        reusing the existing split mechanism)
        if (this->next != 0 && this->key(this->permutation[this->permutation.size() - 1]) 
                > ((leafnode *)ptr_from_off(this->next))->highest) {
            this->next = ((leafnode *)ptr_from_off(this->next))->next;
            clflush((char *)&this->next, sizeof(uint64_t), true);
        }

        leafnode *new_sibling = new leafnode(this->level_);
        new_sibling->writeLock();
        uint64_t split_key;
        this->split_into_inter(new_sibling, split_key);

        leafnode *nl = reinterpret_cast<leafnode *> (this);
        leafnode *nr = reinterpret_cast<leafnode *> (new_sibling);

        permuter perml = nl->permutation;
        int width = perml.size();
        // Removing mid-1 entry
        perml.set_size(width - (nr->permutation.size() + 1));

        if (width != LEAF_WIDTH)
            perml.exchange(perml.size(), LEAF_WIDTH - 1);

        nl->permutation = perml.value();
        clflush((char *)(&nl->permutation), sizeof(permuter), true);

        if (key < split_key) {
            kx_.p = nl->permutation.back();
            if (nl->entry[kx_.p].value != 0)
                isOverWrite = true;

            nl->assign(kx_.p, key, (void *)pmemobj_oid(value).off);
            clflush((char *)(&nl->entry[kx_.p]), sizeof(kv), true);

            permuter cp = nl->permutation.value();
            cp.insert_from_back(kx_.i);
            fence();
            nl->permutation = cp.value();
            clflush((char *)(&nl->permutation), sizeof(permuter), true);

            ret = nl;
        } else {
            kx_ = nr->key_lower_bound_by(key);
            kx_.p = nr->permutation.back();
            nr->assign(kx_.p, key, (void *)pmemobj_oid(value).off);
            clflush((char *)(&nr->entry[kx_.p]), sizeof(kv), true);

            permuter cp = nr->permutation.value();
            cp.insert_from_back(kx_.i);
            fence();
            nr->permutation = cp.value();
            clflush((char *)(&nr->permutation), sizeof(permuter), true);

            ret = nr;
        }

        // lock coupling (hand-over-hand locking)
        if (child != NULL) {
            ((leafnode *)ptr_from_off(child->next))->writeUnlock(false);
            child->writeUnlock(child_isOverWrite);
        }

        if (depth > 0) {
            key_indexed_position pkx_;
            leafnode *p = correct_layer_root(root, lv, depth, pkx_);
            if (p->value(pkx_.p) == pmemobj_oid(this).off) {
                leafnode *new_root = new leafnode(this, split_key, new_sibling, level_ + 1);
                clflush((char *) new_root, sizeof(leafnode), true);
                p->entry[pkx_.p].value = pmemobj_oid(new_root).off;
                clflush((char *) &p->entry[pkx_.p].value, sizeof(uint64_t), true);
                p->writeUnlock(false);

                ((leafnode *)ptr_from_off(this->next))->writeUnlock(false);
                this->writeUnlock(isOverWrite);
            } else {
                root = p;
                t->split(ptr_from_off(p->entry[pkx_.p].value), root, depth, lv, split_key, new_sibling, level_ + 1, this, isOverWrite);
            }
        } else {
            if (t->root() == pmemobj_oid(this).off) {
                leafnode *new_root = new leafnode(this, split_key, new_sibling, level_ + 1);
                clflush((char *) new_root, sizeof(leafnode), true);
                t->setNewRoot(new_root);

                ((leafnode *)ptr_from_off(this->next))->writeUnlock(false);
                this->writeUnlock(isOverWrite);
            } else {
                t->split(NULL, NULL, 0, NULL, split_key, new_sibling, level_ + 1, this, isOverWrite);
            }
        }
    }

    return ret;
}

int leafnode::inter_delete(masstree *t, void *root, uint32_t depth, leafvalue *lv, key_indexed_position &kx_, ThreadInfo &threadInfo)
{
    int ret, merge_state;

    // permutation based remove
    if (this->permutation.size() >= LEAF_THRESHOLD) {
        permuter cp;
        if (kx_.i >= 0) {
            cp = this->permutation.value();
            cp.remove_to_back(kx_.i);
            fence();
            this->permutation = cp.value();
            clflush((char *)(&this->permutation), sizeof(permuter), true);
        }

        this->writeUnlock(false);
    } else {
        // Underflow
        // Merge
        permuter cp;
        leafnode *nl, *nr;
        nr = reinterpret_cast<leafnode *> (this);

        if (depth > 0) {
            key_indexed_position pkx_;
            leafnode *p = correct_layer_root(root, lv, depth, pkx_);
            if (p->value(pkx_.p) == pmemobj_oid(nr).off) {
                kx_.i = 16;
                p->writeUnlock(false);
                nr->writeUnlockObsolete();
                threadInfo.getEpoche().markNodeForDeletion(nr, threadInfo);
                return (ret = kx_.i);
            } else {
                nl = search_for_leftsibling(&p->entry[pkx_.p].value, nr->highest ? nr->highest - 1 : nr->highest, nr->level_, nr);
                merge_state = t->merge(ptr_from_off(p->entry[pkx_.p].value), root, depth, lv, nr->highest, nr->level_ + 1, threadInfo);
            }
        } else {
            if (t->root() == pmemobj_oid(nr).off) {
                kx_.i = 16;
                nr->writeUnlockObsolete();
                threadInfo.getEpoche().markNodeForDeletion(nr, threadInfo);
                return (ret = kx_.i);
            } else {
                nl = search_for_leftsibling(t->root_dp(), nr->highest ? nr->highest - 1 : nr->highest, nr->level_, nr);
                merge_state = t->merge(NULL, NULL, 0, NULL, nr->highest, nr->level_ + 1, threadInfo);
            }
        }

        // Final step for internal node reclamation
        if (merge_state >= 0 && merge_state < 16) {
            nl->next = nr->next;
            clflush((char *)(&nl->next), sizeof(leafnode *), true);
        } else if (merge_state == 16) {
            kx_.i = 16;
        }

        if (nl != nr) {
            if ((merge_state >= 0 && merge_state < 16) || merge_state == 16) {
                nr->writeUnlockObsolete();
                threadInfo.getEpoche().markNodeForDeletion(nr, threadInfo);
            } else {
                nr->writeUnlock(false);
            }
            nl->writeUnlock(false);
        } else {
            if ((merge_state >= 0 && merge_state < 16) || merge_state == 16) {
                nr->writeUnlockObsolete();
                threadInfo.getEpoche().markNodeForDeletion(nr, threadInfo);
            } else {
                nr->writeUnlock(false);
            }
        }
    }

    return (ret = kx_.i);
}

void masstree::split(void *left, void *root, uint32_t depth, leafvalue *lv,
        uint64_t key, void *right, uint32_t level, void *child, bool isOverWrite)
{
    leafnode *p = NULL;
    key_indexed_position kx_;
    leafnode *next = NULL;
    uint64_t snapshot_v = 0;

    int needRestart;
    uint64_t v;

    if (depth > 0) {
        if (level > reinterpret_cast<leafnode *>(left)->level())
            return ;
        p = reinterpret_cast<leafnode *> (left);
        reinterpret_cast<leafnode *> (root)->writeUnlock(false);
    } else {
        if (level > reinterpret_cast<leafnode *>(ptr_from_off(root_))->level())
            return ;
        p = reinterpret_cast<leafnode *> (ptr_from_off(root_));
    }

from_root:
    while (p->level() > level) {
inter_retry:
        next = p->advance_to_key(key);
        if (next != p) {
            p = next;
            goto inter_retry;
        }

        v = p->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED)
                goto inter_retry;
            else {
                if (depth > 0)
                    p = reinterpret_cast<leafnode *> (left);
                else
                    p = reinterpret_cast<leafnode *> (ptr_from_off(root_));
                goto from_root;
            }
        }

        p->prefetch();
        fence();

        kx_ = p->key_lower_bound(key);

        if (kx_.i >= 0)
            snapshot_v = p->value(kx_.p);
        else
            snapshot_v = p->leftmost();

        p->checkOrRestart(v, needRestart);
        if (needRestart)
            goto inter_retry;
        else
            p = reinterpret_cast<leafnode *> (ptr_from_off(snapshot_v));
    }

leaf_retry:
    next = p->advance_to_key(key);
    if (next != p) {
        p = next;
        goto leaf_retry;
    }

    p->writeLockOrRestart(needRestart);
    if (needRestart) {
        if (needRestart == LOCKED)
            goto leaf_retry;
        else {
            if (depth > 0)
                p = reinterpret_cast<leafnode *> (left);
            else
                p = reinterpret_cast<leafnode *> (ptr_from_off(root_));
            goto from_root;
        }
    }

    next = p->advance_to_key(key);
    if (next != p) {
        p->writeUnlock(false);
        p = next;
        goto leaf_retry;
    }

    p->prefetch();
    fence();

    kx_ = p->key_lower_bound_by(key);
    if (kx_.p >= 0 || key == p->highest_()) {
        p->writeUnlock(false);
        reinterpret_cast<leafnode *> (right)->writeUnlock(false);
        reinterpret_cast<leafnode *> (child)->writeUnlock(false);
        return;
    }

    if (!p->inter_insert(this, root, depth, lv, key, right, kx_, 
                reinterpret_cast<leafnode *> (child), isOverWrite)) {
        split(left, root, depth, lv, key, right, level, child, isOverWrite);
    }
}

int masstree::merge(void *left, void *root, uint32_t depth, leafvalue *lv, uint64_t key, uint32_t level, ThreadInfo &threadInfo)
{
    leafnode *p = NULL;
    key_indexed_position kx_;
    leafnode *next = NULL;
    uint64_t snapshot_v = 0;

    int needRestart;
    uint64_t v;

    if (depth > 0) {
        //if (level > reinterpret_cast<leafnode *>(left)->level())
        //    return ;
        p = reinterpret_cast<leafnode *> (left);
        reinterpret_cast<leafnode *> (root)->writeUnlock(false);
    } else {
        //if (level > reinterpret_cast<leafnode *>(this->root_)->level())
        //    return ;
        p = reinterpret_cast<leafnode *> (ptr_from_off(this->root_));
    }

from_root:
    while (p->level() > level) {
inter_retry:
        next = p->advance_to_key(key);
        if (next != p) {
            p = next;
            goto inter_retry;
        }

        v = p->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED)
                goto inter_retry;
            else {
                if (depth > 0)
                    p = reinterpret_cast<leafnode *> (left);
                else
                    p = reinterpret_cast<leafnode *> (ptr_from_off(this->root_));
                goto from_root;
            }
        }

        p->prefetch();
        fence();

        kx_ = p->key_lower_bound(key);

        if (kx_.i >= 0)
            snapshot_v = p->value(kx_.p);
        else
            snapshot_v = p->leftmost();

        p->checkOrRestart(v, needRestart);
        if (needRestart)
            goto inter_retry;
        else
            p = reinterpret_cast<leafnode *> (ptr_from_off(snapshot_v));
    }

leaf_retry:
    next = p->advance_to_key(key);
    if (next != p) {
        p = next;
        goto leaf_retry;
    }

    p->writeLockOrRestart(needRestart);
    if (needRestart) {
        if (needRestart == LOCKED)
            goto leaf_retry;
        else {
            if (depth > 0)
                p = reinterpret_cast<leafnode *> (left);
            else
                p = reinterpret_cast<leafnode *> (ptr_from_off(this->root_));
            goto from_root;
        }
    }

    next = p->advance_to_key(key);
    if (next != p) {
        p->writeUnlock(false);
        p = next;
        goto leaf_retry;
    }

    p->prefetch();
    fence();

    kx_ = p->key_lower_bound(key);

    return p->inter_delete(this, root, depth, lv, kx_, threadInfo);
}

uint64_t masstree::raddr_from_off(uint64_t off)
{
    if (off == 0) return 0;
    else return (remote_start_addr + off);
}

std::tuple<bool, void *, uint64_t> masstree::get(uint64_t key, uint32_t lkey, 
        struct ibv_qp *qp, char *buf_ptr, uint32_t rkey, struct ibv_cq *cq, 
        uint64_t root_raddr, int rank) //ThreadInfo &threadEpocheInfo
{
//    EpocheGuard epocheGuard(threadEpocheInfo);

    int buf_idx = 0;
    uint64_t raddr = 0;
    leafnode *recv_buf_ptr = (leafnode *)buf_ptr;

    void *root = NULL;
    key_indexed_position kx_;
    leafnode *next = NULL;
    uint64_t snapshot_v = 0;

    int needRestart;
    uint64_t v;

restart:
    // root_raddr is the address to masstree structure
    post_read_signaled_blocking(sizeof(masstree), lkey, 0, qp,
            (char *)&recv_buf_ptr[buf_idx], root_raddr, rkey, cq);
    memcpy(this, &recv_buf_ptr[buf_idx], sizeof(masstree));
    raddr = raddr_from_off(this->root_);

    root = &recv_buf_ptr[buf_idx];
    post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)root, raddr, rkey, cq);
    leafnode *p = reinterpret_cast<leafnode *> (root);
    while (p->level() != 0) {
inter_retry:
        next = p->advance_to_key(this, key, raddr, buf_idx, lkey, qp, recv_buf_ptr, rkey, cq);
        if (next != p) {
            p = next;
            goto inter_retry;
        }

        v = p->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED) {
                post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
                goto inter_retry;
            } else {
                goto restart;
            }
        }

        post_read_signaled_blocking(sizeof(leafnode) - LEAF_HEADER_SIZE, lkey, 0, qp,
                (char *)p + LEAF_HEADER_SIZE, raddr + LEAF_HEADER_SIZE, rkey, cq);

        kx_ = p->key_lower_bound(key);

        if (kx_.i >= 0)
            snapshot_v = p->value(kx_.p);
        else
            snapshot_v = p->leftmost();

        post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
        p->checkOrRestart(v, needRestart);
        if (needRestart) {
            goto inter_retry;
        } else {
            raddr = raddr_from_off(snapshot_v);
            post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
            p = reinterpret_cast<leafnode *> (&recv_buf_ptr[buf_idx]);
        }
    }

    leafnode *l = reinterpret_cast<leafnode *> (p);
leaf_retry:
    next = l->advance_to_key(this, key, raddr, buf_idx, lkey, qp, recv_buf_ptr, rkey, cq);
    if (next != l) {
        l = next;
        goto leaf_retry;
    }

    v = l->readLockOrRestart(needRestart);
    if (needRestart) {
        if (needRestart == LOCKED) {
            post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
            goto leaf_retry;
        } else {
            goto restart;
        }
    }

    post_read_signaled_blocking(sizeof(leafnode) - LEAF_HEADER_SIZE, lkey, 0, qp,
            (char *)l + LEAF_HEADER_SIZE, raddr + LEAF_HEADER_SIZE, rkey, cq);

    kx_ = l->key_lower_bound_by(key);

    if (kx_.p >= 0)
        snapshot_v = l->value(kx_.p);
    else
        snapshot_v = 0;

    post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
    l->checkOrRestart(v, needRestart);
    if (needRestart)
        goto leaf_retry;
    else {
        if (!snapshot_v) {
            next = l->advance_to_key(this, key, raddr, buf_idx, lkey, qp, recv_buf_ptr, rkey, cq);
            if (next != l) {
                l = next;
                goto leaf_retry;
            }
#if 0
            printf("should not enter here\n");
            printf("key = %lu, searched key = %lu, key index = %d\n", key, l->key(kx_.p), kx_.p);
            permuter cp = l->permute();
            for (int i = 0; i < cp.size(); i++) {
                printf("key = %lu\n", l->key(cp[i]));
            }

            if (l->next_()) {
                cp = l->next_()->permute();
                printf("next high key = %lu\n", l->next_()->highest_());
                for (int i = 0; i < cp.size(); i++) {
                    printf("next key = %lu\n", l->next_()->key(cp[i]));
                }
            }
#endif
        }

        return std::make_tuple(false, nullptr, snapshot_v);
    }
}

std::tuple<bool, void *, uint64_t, uint32_t> masstree::get(char *key, size_t key_len, 
        uint32_t lkey, struct ibv_qp *qp, char *buf_ptr, uint32_t rkey, struct ibv_cq *cq, 
        uint64_t root_raddr, int rank) //ThreadInfo &threadEpocheInfo
{
//    EpocheGuard epocheGuard(threadEpocheInfo);

    int buf_idx = 0;
    uint64_t raddr = 0;
    leafnode *recv_buf_ptr = (leafnode *)buf_ptr;

    void *root = NULL;
    key_indexed_position kx_;
    uint32_t depth = 0;
    leafnode *next = NULL;
    uint64_t snapshot_v = 0;
    uint32_t valLen = 0;

    int needRestart;
    uint64_t v;

    leafvalue *lv = make_leaf(key, key_len, 0, 0);

restart:
    // root_raddr is the address to masstree structure
    post_read_signaled_blocking(sizeof(masstree), lkey, 0, qp,
            (char *)&recv_buf_ptr[buf_idx], root_raddr, rkey, cq);
    memcpy(this, &recv_buf_ptr[buf_idx], sizeof(masstree));
    raddr = raddr_from_off(this->root_);
    
    depth = 0;
    root = &recv_buf_ptr[buf_idx];
    post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)root, raddr, rkey, cq);
    leafnode *p = reinterpret_cast<leafnode *> (root);
from_root:
    while (p->level() != 0) {
inter_retry:
        next = p->advance_to_key(this, lv->fkey[depth], raddr, buf_idx, lkey, qp, recv_buf_ptr, rkey, cq);
        if (next != p) {
            p = next;
            goto inter_retry;
        }

        v = p->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED) {
                post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
                goto inter_retry;
            } else {
                goto restart;
            }
        }

        post_read_signaled_blocking(sizeof(leafnode) - LEAF_HEADER_SIZE, lkey, 0, qp,
                (char *)p + LEAF_HEADER_SIZE, raddr + LEAF_HEADER_SIZE, rkey, cq);

        kx_ = p->key_lower_bound(lv->fkey[depth]);

        if (kx_.i >= 0)
            snapshot_v = p->value(kx_.p);
        else
            snapshot_v = p->leftmost();

        post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
        p->checkOrRestart(v, needRestart);
        if (needRestart) {
            goto inter_retry;
        } else {
            raddr = raddr_from_off(snapshot_v);
            post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
            p = reinterpret_cast<leafnode *> (&recv_buf_ptr[buf_idx]);
        }
    }

    leafnode *l = reinterpret_cast<leafnode *> (p);
leaf_retry:
    next = l->advance_to_key(this, lv->fkey[depth], raddr, buf_idx, lkey, qp, recv_buf_ptr, rkey, cq);
    if (next != l) {
        l = next;
        goto leaf_retry;
    }

    v = l->readLockOrRestart(needRestart);
    if (needRestart) {
        if (needRestart == LOCKED) {
            post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
            goto leaf_retry;
        } else {
            goto restart;
        }
    }

    post_read_signaled_blocking(sizeof(leafnode) - LEAF_HEADER_SIZE, lkey, 0, qp,
            (char *)l + LEAF_HEADER_SIZE, raddr + LEAF_HEADER_SIZE, rkey, cq);

    kx_ = l->key_lower_bound_by(lv->fkey[depth]);
    if (kx_.p >= 0) {
        snapshot_v = l->value(kx_.p);
        if (!IS_LV(snapshot_v)) {
            // If there is additional layer, traverse B+tree in the next layer
            post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
            l->checkOrRestart(v, needRestart);
            if (needRestart)
                goto leaf_retry;
            else {
                raddr = raddr_from_off(snapshot_v);
                post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
                p = l;
                depth++;
                goto from_root;
            }
        } else {
            snapshot_v = (uint64_t)(LV_PTR(snapshot_v));
        }
    } else {
        snapshot_v = 0;
    }

    leafvalue *ret_lv = NULL;
    if (snapshot_v) {
        ret_lv = (leafvalue *) &recv_buf_ptr[buf_idx];
        post_read_signaled_blocking(sizeof(leafvalue) + lv->key_len, lkey, 0, qp, (char *)ret_lv, raddr_from_off(snapshot_v), rkey, cq);

        if (ret_lv->key_len == lv->key_len && memcmp(ret_lv->fkey, lv->fkey, lv->key_len) == 0) {
            snapshot_v = ret_lv->value;
            valLen = ret_lv->val_len;
            // Presume recv_buf_ptr[3] is empty space
            post_read_signaled_blocking(valLen, lkey, 0, qp, (char *)&recv_buf_ptr[3], raddr_from_off(snapshot_v), rkey, cq);
        } else {
            snapshot_v = 0;
            valLen = 0;
        }
    } 

    post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
    l->checkOrRestart(v, needRestart);
    if (needRestart)
        goto leaf_retry;
    else {
        if (snapshot_v != 0) {
            memmove(buf_ptr, (char *)&recv_buf_ptr[3], valLen);
        } else {
            next = l->advance_to_key(this, lv->fkey[depth], raddr, buf_idx, lkey, qp, recv_buf_ptr, rkey, cq);
            if (next != l) {
                l = next;
                goto leaf_retry;
            }
        }

        return std::make_tuple(false, nullptr, snapshot_v, valLen);
    }
}

void leafnode::get_range(masstree *t, leafvalue * &lv, int num, int &count, std::pair<uint64_t, uint32_t> *buf, 
        leafnode *root, uint32_t depth, uint32_t lkey, struct ibv_qp *qp, char *buf_ptr,
        uint32_t rkey, struct ibv_cq *cq)
{
    int buf_idx = 0;
    uint64_t raddr = 0;
    leafnode *recv_buf_ptr = (leafnode *)buf_ptr;

    key_indexed_position kx_;
    leafnode *next = NULL;
    uint64_t snapshot_v = 0, snapshot_n = 0;
    permuter perm;
    int backup, prev_count = count;

    int needRestart;
    uint64_t v;

restart:
    count = prev_count;
    raddr = (uint64_t)root;
    post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, 
            (char *)&recv_buf_ptr[buf_idx], raddr, rkey, cq);
    leafnode *p = reinterpret_cast<leafnode *> (&recv_buf_ptr[buf_idx]);
    while (p->level() != 0) {
inter_retry:
        next = p->advance_to_key(t, lv->fkey[depth], raddr, buf_idx, lkey, qp, recv_buf_ptr, rkey, cq);
        if (next != p) {
            p = next;
            goto inter_retry;
        }

        v = p->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED) {
                post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, 
                        (char *)p, raddr, rkey, cq);
                goto inter_retry;
            } else {
                goto restart;
            }
        }

        post_read_signaled_blocking(sizeof(leafnode) - LEAF_HEADER_SIZE, lkey, 0, qp,
                (char *)p + LEAF_HEADER_SIZE, raddr + LEAF_HEADER_SIZE, rkey, cq);

        kx_ = p->key_lower_bound(lv->fkey[depth]);

        if (kx_.i >= 0)
            snapshot_v = p->value(kx_.p);
        else
            snapshot_v = p->leftmost();

        post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
        p->checkOrRestart(v, needRestart);
        if (needRestart) {
            goto inter_retry;
        } else {
            raddr = t->raddr_from_off(snapshot_v);
            post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
            p = reinterpret_cast<leafnode *> (&recv_buf_ptr[buf_idx]);
        }
    }

    leafnode *l = reinterpret_cast<leafnode *> (p);
    while (count < num) {
leaf_retry:
        backup = count;
        snapshot_n = l->next_();
        perm = l->permute();

        v = l->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED) {
                post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
                goto leaf_retry;
            } else {
                goto restart;
            }
        }

        post_read_signaled_blocking(sizeof(leafnode) - LEAF_HEADER_SIZE, lkey, 0, qp,
                (char *)l + LEAF_HEADER_SIZE, raddr + LEAF_HEADER_SIZE, rkey, cq);

        for (int i = 0; i < perm.size() && count < num; i++) {
            snapshot_v = l->value(perm[i]);
            if (!IS_LV(l->value(perm[i]))) {
                if (l->key(perm[i]) > lv->fkey[depth]) {
                    leafnode *node_backup = (leafnode *) malloc(sizeof(leafnode));
                    memcpy(node_backup, l, sizeof(leafnode));

                    p = reinterpret_cast<leafnode *> (t->raddr_from_off(snapshot_v));
                    leafvalue *smallest = p->smallest_leaf(lv->key_len, lv->value);
                    p->get_range(t, smallest, num, count, buf, p, depth + 1, lkey, qp, buf_ptr, rkey, cq);
                    free(smallest);

                    if (l == &recv_buf_ptr[0])
                        buf_idx = 0;
                    else
                        buf_idx = 1;

                    post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
                    l->checkOrRestart(v, needRestart);
                    if (needRestart) {
                        count = backup;
                        free(node_backup);
                        goto leaf_retry;
                    } else {
                        memcpy(l, node_backup, sizeof(leafnode));
                        free(node_backup);
                    }
                } else if (l->key(perm[i]) == lv->fkey[depth]) {
                    leafnode *node_backup = (leafnode *) malloc(sizeof(leafnode));
                    memcpy(node_backup, l, sizeof(leafnode));

                    p = reinterpret_cast<leafnode *> (t->raddr_from_off(snapshot_v));
                    p->get_range(t, lv, num, count, buf, p, depth + 1, lkey, qp, buf_ptr, rkey, cq);

                    if (l == &recv_buf_ptr[0])
                        buf_idx = 0;
                    else
                        buf_idx = 1;

                    post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
                    l->checkOrRestart(v, needRestart);
                    if (needRestart) {
                        count = backup;
                        free(node_backup);
                        goto leaf_retry;
                    } else {
                        memcpy(l, node_backup, sizeof(leafnode));
                        free(node_backup);
                    }
                }
            } else {
                leafvalue *ret_lv = (leafvalue *) &recv_buf_ptr[(buf_idx == 1 ? buf_idx - 1 : buf_idx + 1)];
                snapshot_v = (uint64_t)(LV_PTR(snapshot_v));
                if (l->key(perm[i]) > lv->fkey[depth]) {
                    post_read_signaled_blocking(sizeof(leafvalue), lkey, 0, qp,
                            (char *)ret_lv, t->raddr_from_off(snapshot_v), rkey, cq);
                    buf[count++] = std::make_pair(ret_lv->value, ret_lv->val_len);
                } else if (l->key(perm[i]) == lv->fkey[depth]) {
                    post_read_signaled_blocking(sizeof(leafvalue) + lv->key_len, lkey, 0,
                            qp, (char *)ret_lv, t->raddr_from_off(snapshot_v), rkey, cq);
                    if (memcmp(ret_lv->fkey, lv->fkey, lv->key_len) >= 0)
                        buf[count++] = std::make_pair(ret_lv->value, ret_lv->val_len);
                }
            }
        }

        post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
        l->checkOrRestart(v, needRestart);
        if (needRestart) {
            count = backup;
            continue;
        } else {
            if (snapshot_n == 0) {
                break;
            } else {
                raddr = t->raddr_from_off(snapshot_n);
                post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)&recv_buf_ptr[buf_idx], raddr, rkey, cq);
                l = reinterpret_cast<leafnode *> (&recv_buf_ptr[buf_idx]);
            }
        }
    }
}

int masstree::scan(char *min, size_t key_len, int num, std::pair<uint64_t, uint32_t> *buf, uint32_t lkey, struct ibv_qp *qp,
        char *buf_ptr, uint32_t rkey, struct ibv_cq *cq, uint64_t root_raddr, int rank) //ThreadInfo &threadEpocheInfo
{
//    EpocheGuard epocheGuard(threadEpocheInfo);

    int buf_idx = 0;
    uint64_t raddr = 0;
    leafnode *recv_buf_ptr = (leafnode *)buf_ptr;

    void *root = NULL;
    key_indexed_position kx_;
    uint32_t depth = 0;
    leafnode *next = NULL;
    uint64_t snapshot_v = 0, snapshot_n = 0;
    permuter perm;
    int count, backup;

    leafvalue *lv = make_leaf(min, key_len, 0, 0);

    int needRestart;
    uint64_t v;

restart:
    depth = 0;
    count = 0;

    // root_raddr is the address to masstree structure
    post_read_signaled_blocking(sizeof(masstree), lkey, 0, qp,
            (char *)&recv_buf_ptr[buf_idx], root_raddr, rkey, cq);
    memcpy(this, &recv_buf_ptr[buf_idx], sizeof(masstree));
    raddr = raddr_from_off(this->root_);

    root = &recv_buf_ptr[buf_idx];
    post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)root, raddr, rkey, cq);
    leafnode *p = reinterpret_cast<leafnode *> (root);
    while (p->level() != 0) {
inter_retry:
        next = p->advance_to_key(this, lv->fkey[depth], raddr, buf_idx, lkey, qp, recv_buf_ptr, rkey, cq);
        if (next != p) {
            p = next;
            goto inter_retry;
        }

        v = p->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED) {
                post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
                goto inter_retry;
            } else {
                goto restart;
            }
        }

        post_read_signaled_blocking(sizeof(leafnode) - LEAF_HEADER_SIZE, lkey, 0, qp,
                (char *)p + LEAF_HEADER_SIZE, raddr + LEAF_HEADER_SIZE, rkey, cq);

        kx_ = p->key_lower_bound(lv->fkey[depth]);

        if (kx_.i >= 0)
            snapshot_v = p->value(kx_.p);
        else
            snapshot_v = p->leftmost();

        post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
        p->checkOrRestart(v, needRestart);
        if (needRestart) {
            goto inter_retry;
        } else {
            raddr = raddr_from_off(snapshot_v);
            post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
            p = reinterpret_cast<leafnode *> (&recv_buf_ptr[buf_idx]);
        }
    }

    leafnode *l = reinterpret_cast<leafnode *> (p);
    while (count < num) {
leaf_retry:
        backup = count;
        snapshot_n = l->next_();
        perm = l->permute();

        v = l->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED) {
                post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
                goto leaf_retry;
            } else {
                goto restart;
            }
        }

        post_read_signaled_blocking(sizeof(leafnode) - LEAF_HEADER_SIZE, lkey, 0, qp,
                (char *)l + LEAF_HEADER_SIZE, raddr + LEAF_HEADER_SIZE, rkey, cq);

        for (int i = 0; i < perm.size() && count < num; i++) {
            snapshot_v = l->value(perm[i]);
            if (!IS_LV(l->value(perm[i]))) {
                if (l->key(perm[i]) > lv->fkey[depth]) {
                    leafnode *node_backup = (leafnode *) malloc(sizeof(leafnode));
                    memcpy(node_backup, l, sizeof(leafnode));

                    p = reinterpret_cast<leafnode *> (raddr_from_off(snapshot_v));
                    leafvalue *smallest = p->smallest_leaf(lv->key_len, lv->value);
                    p->get_range(this, smallest, num, count, buf, p, depth + 1, lkey, qp, buf_ptr, rkey, cq);
                    free(smallest);

                    if (l == &recv_buf_ptr[0])
                        buf_idx = 0;
                    else
                        buf_idx = 1;

                    post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
                    l->checkOrRestart(v, needRestart);
                    if (needRestart) {
                        count = backup;
                        free(node_backup);
                        goto leaf_retry;
                    } else {
                        memcpy(l, node_backup, sizeof(leafnode));
                        free(node_backup);
                    }
                } else if (l->key(perm[i]) == lv->fkey[depth]) {
                    leafnode *node_backup = (leafnode *) malloc(sizeof(leafnode));
                    memcpy(node_backup, l, sizeof(leafnode));

                    p = reinterpret_cast<leafnode *> (raddr_from_off(snapshot_v));
                    p->get_range(this, lv, num, count, buf, p, depth + 1, lkey, qp, buf_ptr, rkey, cq);

                    if (l == &recv_buf_ptr[0])
                        buf_idx = 0;
                    else
                        buf_idx = 1;

                    post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
                    l->checkOrRestart(v, needRestart);
                    if (needRestart) {
                        count = backup;
                        free(node_backup);
                        goto leaf_retry;
                    } else {
                        memcpy(l, node_backup, sizeof(leafnode));
                        free(node_backup);
                    }
                }
            } else {
                leafvalue *ret_lv = (leafvalue *) &recv_buf_ptr[(buf_idx == 1 ? buf_idx - 1 : buf_idx + 1)];
                snapshot_v = (uint64_t)(LV_PTR(snapshot_v));
                if (l->key(perm[i]) > lv->fkey[depth]) {
                    post_read_signaled_blocking(sizeof(leafvalue), lkey, 0, qp, 
                            (char *)ret_lv, raddr_from_off(snapshot_v), rkey, cq);
                    buf[count++] = std::make_pair(ret_lv->value, ret_lv->val_len);
                } else if (l->key(perm[i]) == lv->fkey[depth]) {
                    post_read_signaled_blocking(sizeof(leafvalue) + lv->key_len, lkey, 0, qp, 
                            (char *)ret_lv, raddr_from_off(snapshot_v), rkey, cq);
                    if (memcmp(ret_lv->fkey, lv->fkey, lv->key_len) >= 0)
                        buf[count++] = std::make_pair(ret_lv->value, ret_lv->val_len);
                }
            }
        }

        post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
        l->checkOrRestart(v, needRestart);
        if (needRestart) {
            count = backup;
            continue;
        } else {
            if (snapshot_n == 0) {
                break;
            } else {
                raddr = raddr_from_off(snapshot_n);
                post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)&recv_buf_ptr[buf_idx], raddr, rkey, cq);
                l = reinterpret_cast<leafnode *> (&recv_buf_ptr[buf_idx]);
            }
        }
    }

    free(lv);
    return count;
}

int masstree::scan(uint64_t min, int num, uint64_t *buf, uint64_t lkey, struct ibv_qp *qp,
        char *buf_ptr, uint32_t rkey, struct ibv_cq *cq, uint64_t root_raddr, int rank) //ThreadInfo &threadEpocheInfo
{
//    EpocheGuard epocheGuard(threadEpocheInfo);

    int buf_idx = 0;
    uint64_t raddr = 0;
    leafnode *recv_buf_ptr = (leafnode *)buf_ptr;

    void *root = NULL;
    key_indexed_position kx_;
    leafnode *next = NULL;
    uint64_t snapshot_v = 0;
    uint64_t snapshot_n = 0;
    permuter perm;
    int count, backup;

    int needRestart;
    uint64_t v;

restart:
    count = 0;

    // root_raddr is the address to masstree structure
    post_read_signaled_blocking(sizeof(masstree), lkey, 0, qp,
            (char *)&recv_buf_ptr[buf_idx], root_raddr, rkey, cq);
    memcpy(this, &recv_buf_ptr[buf_idx], sizeof(masstree));
    raddr = raddr_from_off(this->root_);

    root = &recv_buf_ptr[buf_idx];
    post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)root, raddr, rkey, cq);
    leafnode *p = reinterpret_cast<leafnode *> (root);
    while (p->level() != 0) {
inter_retry:
        next = p->advance_to_key(this, min, raddr, buf_idx, lkey, qp, recv_buf_ptr, rkey, cq);
        if (next != p) {
            p = next;
            goto inter_retry;
        }

        v = p->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED) {
                post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
                goto inter_retry;
            } else {
                goto restart;
            }
        }

        post_read_signaled_blocking(sizeof(leafnode) - LEAF_HEADER_SIZE, lkey, 0, qp,
                (char *)p + LEAF_HEADER_SIZE, raddr + LEAF_HEADER_SIZE, rkey, cq);

        kx_ = p->key_lower_bound(min);

        if (kx_.i >= 0)
            snapshot_v = p->value(kx_.p);
        else
            snapshot_v = p->leftmost();

        post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
        p->checkOrRestart(v, needRestart);
        if (needRestart) {
            goto inter_retry;
        } else {
            raddr = raddr_from_off(snapshot_v);
            post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)p, raddr, rkey, cq);
            p = reinterpret_cast<leafnode *> (&recv_buf_ptr[buf_idx]);
        }
    }

    leafnode *l = reinterpret_cast<leafnode *> (p);
    while (count < num) {
leaf_retry:
        backup = count;
        snapshot_n = l->next_();
        perm = l->permute();

        v = l->readLockOrRestart(needRestart);
        if (needRestart) {
            if (needRestart == LOCKED) {
                post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
                goto leaf_retry;
            } else {
                goto restart;
            }
        }

        post_read_signaled_blocking(sizeof(leafnode) - LEAF_HEADER_SIZE, lkey, 0, qp,
                (char *)l + LEAF_HEADER_SIZE, raddr + LEAF_HEADER_SIZE, rkey, cq);

        for (int i = 0; i < perm.size() && count < num; i++) {
            snapshot_v = l->value(perm[i]);
            if (l->key(perm[i]) >= min) {
                buf[count++] = (uint64_t)snapshot_v;
            }
        }

        post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)l, raddr, rkey, cq);
        l->checkOrRestart(v, needRestart);
        if (needRestart) {
            count = backup;
            continue;
        } else {
            if (snapshot_n == 0) {
                break;
            } else {
                raddr = raddr_from_off(snapshot_n);
                post_read_signaled_blocking(LEAF_HEADER_SIZE, lkey, 0, qp, (char *)&recv_buf_ptr[buf_idx], raddr, rkey, cq);
                l = reinterpret_cast<leafnode *> (&recv_buf_ptr[buf_idx]);
            }
        }
    }

    return count;
}

}
