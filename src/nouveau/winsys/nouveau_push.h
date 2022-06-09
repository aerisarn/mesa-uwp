#ifndef NOUVEAU_CMD_BUF
#define NOUVEAU_CMD_BUF 1

#include "nouveau_private.h"

#include "nouveau_bo.h"
#include "nouveau_drm.h"
#include "util/u_dynarray.h"

struct nouveau_ws_context;
struct nouveau_ws_device;

struct nouveau_ws_push_bo {
   struct nouveau_ws_bo *bo;
   enum nouveau_ws_bo_map_flags flags;
};

struct nouveau_ws_push {
   struct util_dynarray bos;
   struct nouveau_ws_bo *bo;
   uint32_t *orig_map;
   uint32_t *map;
   uint32_t *end;

   uint32_t *last_size;
};

struct nouveau_ws_push *nouveau_ws_push_new(struct nouveau_ws_device *, uint64_t size);
void nouveau_ws_push_destroy(struct nouveau_ws_push *);
int nouveau_ws_push_submit(struct nouveau_ws_push *, struct nouveau_ws_device *, struct nouveau_ws_context *);
void nouveau_ws_push_ref(struct nouveau_ws_push *, struct nouveau_ws_bo *, enum nouveau_ws_bo_map_flags);
void nouveau_ws_push_reset(struct nouveau_ws_push *);

#ifdef CONFIG_NOUVEAU_DEBUG_PUSH
#define PUSH_PRINTF(p,f,a...) do {                              \
	struct nouveau_ws_push *_ppp = (p);                           \
	uint32_t __o = _ppp->map - (uint32_t *)_ppp->mem.object.map.ptr;  \
	NVIF_DEBUG(&_ppp->mem.object, "%08x: "f, __o * 4, ##a); \
	(void)__o;                                              \
} while(0)
#define PUSH_ASSERT_ON(a,b) WARN((a), b)
#else
#define PUSH_PRINTF(p,f,a...)
#define PUSH_ASSERT_ON(a, b)
#endif

#define PUSH_ASSERT(a,b) do {                                             \
	static_assert(                                                    \
		__builtin_choose_expr(__builtin_constant_p(a), (a), 1), b \
	);                                                                \
	PUSH_ASSERT_ON(!(a), b);                                          \
} while(0)

#define PUSH_DATA__(p,d,f,a...) do {                       \
	struct nouveau_ws_push *_p = (p);                        \
	uint32_t _d = (d);                                      \
	PUSH_ASSERT(_p->map < _p->end, "pushbuf overrun"); \
	PUSH_PRINTF(_p, "%08x"f, _d, ##a);                 \
	*_p->map++ = _d;                                   \
} while(0)

#define PUSH_DATA_(X,p,m,i0,i1,d,s,f,a...) PUSH_DATA__((p), (d), "-> "#m f, ##a)
#define PUSH_DATA(p,d) PUSH_DATA__((p), (d), " data - %s", __func__)

//XXX: error-check this against *real* pushbuffer end?
#define PUSH_RSVD(p,d) do {          \
	struct nouveau_ws_push *__p = (p); \
	__p->end++;                  \
	d;                           \
} while(0)

#ifdef CONFIG_NOUVEAU_DEBUG_PUSH
#define PUSH_DATAp(X,p,m,i,o,d,s,f,a...) do {                                     \
	struct nouveau_ws_push *_pp = (p);                                              \
	const uint32_t *_dd = (d);                                                     \
	uint32_t _s = (s), _i = (i?PUSH_##o##_INC);                                    \
	if (_s--) {                                                               \
		PUSH_DATA_(X, _pp, X##m, i0, i1, *_dd++, 1, "+0x%x", 0);          \
		while (_s--) {                                                    \
			PUSH_DATA_(X, _pp, X##m, i0, i1, *_dd++, 1, "+0x%x", _i); \
			_i += (0?PUSH_##o##_INC);                                 \
		}                                                                 \
	}                                                                         \
} while(0)
#else
#define PUSH_DATAp(X,p,m,i,o,d,s,f,a...) do {                    \
	struct nouveau_ws_push *_p = (p);                              \
	uint32_t _s = (s);                                            \
	PUSH_ASSERT(_p->map + _s <= _p->end, "pushbuf overrun"); \
	memcpy(_p->map, (d), _s << 2);                           \
	_p->map += _s;                                           \
} while(0)
#endif

#define PUSH_1(X,f,ds,n,o,p,s,mA,dA) do {                             \
	PUSH_##o##_HDR((p), s, mA, (ds)+(n));                         \
	PUSH_##f(X, (p), X##mA, 1, o, (dA), ds, "");                  \
} while(0)
#define PUSH_2(X,f,ds,n,o,p,s,mB,dB,mA,dA,a...) do {                  \
	PUSH_ASSERT((mB) - (mA) == (1?PUSH_##o##_INC), "mthd1");      \
	PUSH_1(X, DATA_, 1, (ds) + (n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                  \
} while(0)
#define PUSH_3(X,f,ds,n,o,p,s,mB,dB,mA,dA,a...) do {                  \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd2");      \
	PUSH_2(X, DATA_, 1, (ds) + (n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                  \
} while(0)
#define PUSH_4(X,f,ds,n,o,p,s,mB,dB,mA,dA,a...) do {                  \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd3");      \
	PUSH_3(X, DATA_, 1, (ds) + (n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                  \
} while(0)
#define PUSH_5(X,f,ds,n,o,p,s,mB,dB,mA,dA,a...) do {                  \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd4");      \
	PUSH_4(X, DATA_, 1, (ds) + (n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                  \
} while(0)
#define PUSH_6(X,f,ds,n,o,p,s,mB,dB,mA,dA,a...) do {                  \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd5");      \
	PUSH_5(X, DATA_, 1, (ds) + (n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                  \
} while(0)
#define PUSH_7(X,f,ds,n,o,p,s,mB,dB,mA,dA,a...) do {                  \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd6");      \
	PUSH_6(X, DATA_, 1, (ds) + (n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                  \
} while(0)
#define PUSH_8(X,f,ds,n,o,p,s,mB,dB,mA,dA,a...) do {                  \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd7");      \
	PUSH_7(X, DATA_, 1, (ds) + (n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                  \
} while(0)
#define PUSH_9(X,f,ds,n,o,p,s,mB,dB,mA,dA,a...) do {                  \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd8");      \
	PUSH_8(X, DATA_, 1, (ds) + (n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                  \
} while(0)
#define PUSH_10(X,f,ds,n,o,p,s,mB,dB,mA,dA,a...) do {                 \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd9");      \
	PUSH_9(X, DATA_, 1, (ds) + (n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                  \
} while(0)

#define PUSH_1D(X,o,p,s,mA,dA)                         \
	PUSH_1(X, DATA_, 1, 0, o, (p), s, X##mA, (dA))
#define PUSH_2D(X,o,p,s,mA,dA,mB,dB)                   \
	PUSH_2(X, DATA_, 1, 0, o, (p), s, X##mB, (dB), \
					  X##mA, (dA))
#define PUSH_3D(X,o,p,s,mA,dA,mB,dB,mC,dC)             \
	PUSH_3(X, DATA_, 1, 0, o, (p), s, X##mC, (dC), \
					  X##mB, (dB), \
					  X##mA, (dA))
#define PUSH_4D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD)       \
	PUSH_4(X, DATA_, 1, 0, o, (p), s, X##mD, (dD), \
					  X##mC, (dC), \
					  X##mB, (dB), \
					  X##mA, (dA))
#define PUSH_5D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE) \
	PUSH_5(X, DATA_, 1, 0, o, (p), s, X##mE, (dE), \
					  X##mD, (dD), \
					  X##mC, (dC), \
					  X##mB, (dB), \
					  X##mA, (dA))
#define PUSH_6D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF) \
	PUSH_6(X, DATA_, 1, 0, o, (p), s, X##mF, (dF),       \
					  X##mE, (dE),       \
					  X##mD, (dD),       \
					  X##mC, (dC),       \
					  X##mB, (dB),       \
					  X##mA, (dA))
#define PUSH_7D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG) \
	PUSH_7(X, DATA_, 1, 0, o, (p), s, X##mG, (dG),             \
					  X##mF, (dF),             \
					  X##mE, (dE),             \
					  X##mD, (dD),             \
					  X##mC, (dC),             \
					  X##mB, (dB),             \
					  X##mA, (dA))
#define PUSH_8D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG,mH,dH) \
	PUSH_8(X, DATA_, 1, 0, o, (p), s, X##mH, (dH),                   \
					  X##mG, (dG),                   \
					  X##mF, (dF),                   \
					  X##mE, (dE),                   \
					  X##mD, (dD),                   \
					  X##mC, (dC),                   \
					  X##mB, (dB),                   \
					  X##mA, (dA))
#define PUSH_9D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG,mH,dH,mI,dI) \
	PUSH_9(X, DATA_, 1, 0, o, (p), s, X##mI, (dI),                         \
					  X##mH, (dH),                         \
					  X##mG, (dG),                         \
					  X##mF, (dF),                         \
					  X##mE, (dE),                         \
					  X##mD, (dD),                         \
					  X##mC, (dC),                         \
					  X##mB, (dB),                         \
					  X##mA, (dA))
#define PUSH_10D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG,mH,dH,mI,dI,mJ,dJ) \
	PUSH_10(X, DATA_, 1, 0, o, (p), s, X##mJ, (dJ),                               \
					   X##mI, (dI),                               \
					   X##mH, (dH),                               \
					   X##mG, (dG),                               \
					   X##mF, (dF),                               \
					   X##mE, (dE),                               \
					   X##mD, (dD),                               \
					   X##mC, (dC),                               \
					   X##mB, (dB),                               \
					   X##mA, (dA))

#define PUSH_1P(X,o,p,s,mA,dp,ds)                       \
	PUSH_1(X, DATAp, ds, 0, o, (p), s, X##mA, (dp))
#define PUSH_2P(X,o,p,s,mA,dA,mB,dp,ds)                 \
	PUSH_2(X, DATAp, ds, 0, o, (p), s, X##mB, (dp), \
					   X##mA, (dA))
#define PUSH_3P(X,o,p,s,mA,dA,mB,dB,mC,dp,ds)           \
	PUSH_3(X, DATAp, ds, 0, o, (p), s, X##mC, (dp), \
					   X##mB, (dB), \
					   X##mA, (dA))

#define PUSH_(A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,IMPL,...) IMPL
#define PUSH(A...) PUSH_(A, PUSH_10P, PUSH_10D,          \
			    PUSH_9P , PUSH_9D,           \
			    PUSH_8P , PUSH_8D,           \
			    PUSH_7P , PUSH_7D,           \
			    PUSH_6P , PUSH_6D,           \
			    PUSH_5P , PUSH_5D,           \
			    PUSH_4P , PUSH_4D,           \
			    PUSH_3P , PUSH_3D,           \
			    PUSH_2P , PUSH_2D,           \
			    PUSH_1P , PUSH_1D)(, ##A)

#define PUSH_NVIM(p,c,m,d) do {             \
	struct nouveau_ws_push *__p = (p);        \
	uint32_t __d = (d);                      \
	assert(!(__d & ~0xffff) && "immediate value must be 16 bit"); \
	PUSH_IMMD_HDR(__p, c, m, __d);      \
	__p->map--;                         \
	PUSH_PRINTF(__p, "%08x-> "#m, __d); \
	__p->map++;                         \
} while(0)
#define PUSH_NVSQ(A...) PUSH(MTHD, ##A)
#define PUSH_NV1I(A...) PUSH(1INC, ##A)
#define PUSH_NVNI(A...) PUSH(NINC, ##A)


#define PUSH_NV_1(X,o,p,c,mA,d...) \
       PUSH_##o(p,c,c##_##mA,d)
#define PUSH_NV_2(X,o,p,c,mA,dA,mB,d...) \
       PUSH_##o(p,c,c##_##mA,dA,         \
		    c##_##mB,d)
#define PUSH_NV_3(X,o,p,c,mA,dA,mB,dB,mC,d...) \
       PUSH_##o(p,c,c##_##mA,dA,               \
		    c##_##mB,dB,               \
		    c##_##mC,d)
#define PUSH_NV_4(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                     \
		    c##_##mB,dB,                     \
		    c##_##mC,dC,                     \
		    c##_##mD,d)
#define PUSH_NV_5(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,dD,mE,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                           \
		    c##_##mB,dB,                           \
		    c##_##mC,dC,                           \
		    c##_##mD,dD,                           \
		    c##_##mE,d)
#define PUSH_NV_6(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                                 \
		    c##_##mB,dB,                                 \
		    c##_##mC,dC,                                 \
		    c##_##mD,dD,                                 \
		    c##_##mE,dE,                                 \
		    c##_##mF,d)
#define PUSH_NV_7(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                                       \
		    c##_##mB,dB,                                       \
		    c##_##mC,dC,                                       \
		    c##_##mD,dD,                                       \
		    c##_##mE,dE,                                       \
		    c##_##mF,dF,                                       \
		    c##_##mG,d)
#define PUSH_NV_8(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG,mH,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                                             \
		    c##_##mB,dB,                                             \
		    c##_##mC,dC,                                             \
		    c##_##mD,dD,                                             \
		    c##_##mE,dE,                                             \
		    c##_##mF,dF,                                             \
		    c##_##mG,dG,                                             \
		    c##_##mH,d)
#define PUSH_NV_9(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG,mH,dH,mI,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                                                   \
		    c##_##mB,dB,                                                   \
		    c##_##mC,dC,                                                   \
		    c##_##mD,dD,                                                   \
		    c##_##mE,dE,                                                   \
		    c##_##mF,dF,                                                   \
		    c##_##mG,dG,                                                   \
		    c##_##mH,dH,                                                   \
		    c##_##mI,d)
#define PUSH_NV_10(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG,mH,dH,mI,dI,mJ,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                                                          \
		    c##_##mB,dB,                                                          \
		    c##_##mC,dC,                                                          \
		    c##_##mD,dD,                                                          \
		    c##_##mE,dE,                                                          \
		    c##_##mF,dF,                                                          \
		    c##_##mG,dG,                                                          \
		    c##_##mH,dH,                                                          \
		    c##_##mI,dI,                                                          \
		    c##_##mJ,d)

#define PUSH_NV_(A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,IMPL,...) IMPL
#define PUSH_NV(A...) PUSH_NV_(A, PUSH_NV_10, PUSH_NV_10,       \
				  PUSH_NV_9 , PUSH_NV_9,        \
				  PUSH_NV_8 , PUSH_NV_8,        \
				  PUSH_NV_7 , PUSH_NV_7,        \
				  PUSH_NV_6 , PUSH_NV_6,        \
				  PUSH_NV_5 , PUSH_NV_5,        \
				  PUSH_NV_4 , PUSH_NV_4,        \
				  PUSH_NV_3 , PUSH_NV_3,        \
				  PUSH_NV_2 , PUSH_NV_2,        \
				  PUSH_NV_1 , PUSH_NV_1)(, ##A)

#define PUSH_IMMD(A...) PUSH_NV(NVIM, ##A)
#define PUSH_MTHD(A...) PUSH_NV(NVSQ, ##A)
#define PUSH_1INC(A...) PUSH_NV(NV1I, ##A)
#define PUSH_NINC(A...) PUSH_NV(NVNI, ##A)

#define SUBC_NV902D 3

#define SUBC_NV90B5 4

static inline uint32_t
NVC0_FIFO_PKHDR_SQ(int subc, int mthd, unsigned size)
{
   return 0x20000000 | (size << 16) | (subc << 13) | (mthd >> 2);
}

static inline void
__push_mthd(struct nouveau_ws_push *push, int subc, uint32_t mthd)
{
   push->last_size = push->map;
   *push->map = NVC0_FIFO_PKHDR_SQ(subc, mthd, 0);
   push->map++;
}

#define P_MTHD(push, class, mthd) __push_mthd(push, SUBC_##class, class##_##mthd)

static inline uint32_t
NVC0_FIFO_PKHDR_IL(int subc, int mthd, uint16_t data)
{
   assert(data < 0x2000);
   return 0x80000000 | (data << 16) | (subc << 13) | (mthd >> 2);
}

static inline void
__push_immd(struct nouveau_ws_push *push, int subc, uint32_t mthd, uint32_t val)
{
   push->last_size = push->map;
   *push->map = NVC0_FIFO_PKHDR_IL(subc, mthd, val);
   push->map++;
}

#define P_IMMD(push, class, mthd, args...) do {\
   uint32_t __val; \
   V_##class##_##mthd(__val, args);         \
   __push_immd(push, SUBC_##class, class##_##mthd, __val); \
   } while(0)

static inline uint32_t
NVC0_FIFO_PKHDR_1I(int subc, int mthd, unsigned size)
{
   return 0xa0000000 | (size << 16) | (subc << 13) | (mthd >> 2);
}

static inline void
__push_1inc(struct nouveau_ws_push *push, int subc, uint32_t mthd)
{
   push->last_size = push->map;
   *push->map = NVC0_FIFO_PKHDR_1I(subc, mthd, 0);
   push->map++;
}

#define P_1INC(push, class, mthd) __push_1inc(push, SUBC_##class, class##_##mthd)

static inline uint32_t
NVC0_FIFO_PKHDR_0I(int subc, int mthd, unsigned size)
{
   return 0x60000000 | (size << 16) | (subc << 13) | (mthd >> 2);
}

static inline void
__push_0inc(struct nouveau_ws_push *push, int subc, uint32_t mthd)
{
   push->last_size = push->map;
   *push->map = NVC0_FIFO_PKHDR_0I(subc, mthd, 0);
   push->map++;
}

#define P_0INC(push, class, mthd) __push_0inc(push, SUBC_##class, class##_##mthd)

static inline bool
nvk_push_update_count(struct nouveau_ws_push *push, uint16_t count)
{
   uint32_t last_hdr_val = *push->last_size;

   assert(count <= 0x1fff);
   if (count > 0x1fff)
      return false;

   /* size is encoded at 28:16 */
   uint32_t new_count = (count + (last_hdr_val >> 16)) & 0x1fff;
   bool overflow = new_count < count;
   /* if we would overflow, don't change anything and just let it be */
   assert(!overflow);
   if (overflow)
      return false;

   last_hdr_val &= ~0x1fff0000;
   last_hdr_val |= new_count << 16;
   *push->last_size = last_hdr_val;
   return true;
}

static inline void
P_INLINE_DATA(struct nouveau_ws_push *push, uint32_t value)
{
   if (nvk_push_update_count(push, 1)) {
      /* push new value */
      *push->map = value;
      push->map++;
   }
}

static inline void
P_INLINE_ARRAY(struct nouveau_ws_push *push, const uint32_t *data, int num_dw)
{
   if (nvk_push_update_count(push, num_dw)) {
      /* push new value */
      memcpy(push->map, data, num_dw * 4);
      push->map += num_dw;
   }
}

/* internally used by generated inlines. */
static inline void
nvk_push_val(struct nouveau_ws_push *push, uint32_t idx, uint32_t val)
{
   UNUSED uint32_t last_hdr_val = *push->last_size;
   UNUSED bool is_1inc = (last_hdr_val & 0xe0000000) == 0xa0000000;
   UNUSED bool is_immd = (last_hdr_val & 0xe0000000) == 0x80000000;
   UNUSED uint16_t last_method = (last_hdr_val & 0x1fff) << 2;

   uint16_t distance = push->map - push->last_size - 1;
   if (is_1inc)
      distance = MIN2(1, distance);
   last_method += distance * 4;

   /* can't have empty headers ever */
   assert(last_hdr_val);
   assert(!is_immd);
   assert(last_method == idx);

   P_INLINE_DATA(push, val);
}
#endif
