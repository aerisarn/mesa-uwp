#include <fcntl.h>
#include <string.h>
#include <xf86drm.h>
#include <vector>

#include <gtest/gtest.h>

#include "mme_builder.h"
#include "mme_fermi_sim.h"

#include "nouveau_bo.h"
#include "nouveau_context.h"
#include "nouveau_device.h"

/* nouveau_drm.h isn't C++-friendly */
#define class cls
#include <nouveau_drm.h>
#undef class

#include <xf86drm.h>

#include "nv_push.h"
/* we use Fermi Compute and 2D as interface are identical between Fermi and Volta*/
#include "nvk_cl9097.h"
#include "nvk_cl902d.h"
/* for VOLTA_A */
#include "nvk_clc397.h"

class mme_fermi_sim_test : public ::testing::Test {
public:
   mme_fermi_sim_test();
   ~mme_fermi_sim_test();

   void SetUp();
   void push_macro(uint32_t id, const std::vector<uint32_t>& macro);
   void reset_push();
   void submit_push();
   void test_macro(const mme_builder *b,
                   const std::vector<uint32_t>& macro,
                   const std::vector<uint32_t>& params);

   struct nv_push *p;
   uint64_t data_addr;
   uint32_t *data;
   struct nouveau_ws_device *dev;

private:
   struct nouveau_ws_context *ctx;
   struct nouveau_ws_bo *data_bo;
   struct nouveau_ws_bo *push_bo;
   void *push_map;
   struct nv_push push;
};

mme_fermi_sim_test::mme_fermi_sim_test() :
  data(NULL), dev(NULL), ctx(NULL), data_bo(NULL), push_bo(NULL)
{
   memset(&push, 0, sizeof(push));
}

mme_fermi_sim_test::~mme_fermi_sim_test()
{
   if (push_bo) {
      nouveau_ws_bo_unmap(push_bo, push_map);
      nouveau_ws_bo_destroy(push_bo);
   }
   if (ctx)
      nouveau_ws_context_destroy(ctx);
   if (dev)
      nouveau_ws_device_destroy(dev);
}

#define PUSH_SIZE 64 * 4096
#define DATA_BO_SIZE 4096

void
mme_fermi_sim_test::SetUp()
{
   drmDevicePtr devices[8];
   int max_devices = drmGetDevices2(0, devices, 8);

   int i;
   for (i = 0; i < max_devices; i++) {
      if (devices[i]->available_nodes & 1 << DRM_NODE_RENDER &&
          devices[i]->bustype == DRM_BUS_PCI &&
          devices[i]->deviceinfo.pci->vendor_id == 0x10de) {
         dev = nouveau_ws_device_new(devices[i]);
         if (dev == NULL)
            continue;

         if (dev->info.cls_eng3d < FERMI_A || dev->info.cls_eng3d > VOLTA_A) {
            nouveau_ws_device_destroy(dev);
            dev = NULL;
            continue;
         }

         /* Found a Fermi+ device */
         break;
      }
   }

   /* We need a Fermi+ device */
   ASSERT_TRUE(dev != NULL);

   int ret = nouveau_ws_context_create(dev, &ctx);
   ASSERT_EQ(ret, 0);

   uint32_t data_bo_flags = NOUVEAU_WS_BO_GART | NOUVEAU_WS_BO_MAP;
   data_bo = nouveau_ws_bo_new_mapped(dev, DATA_BO_SIZE, 0,
                                      (nouveau_ws_bo_flags)data_bo_flags,
                                      NOUVEAU_WS_BO_RDWR, (void **)&data);
   ASSERT_TRUE(data_bo != NULL);
   memset(data, 139, DATA_BO_SIZE);
   data_addr = data_bo->offset;

   uint32_t push_bo_flags = NOUVEAU_WS_BO_GART | NOUVEAU_WS_BO_MAP;
   push_bo = nouveau_ws_bo_new_mapped(dev, PUSH_SIZE, 0,
                                      (nouveau_ws_bo_flags)push_bo_flags,
                                      NOUVEAU_WS_BO_WR, &push_map);
   ASSERT_TRUE(push_bo != NULL);
   reset_push();
}

void
mme_fermi_sim_test::reset_push()
{
   nv_push_init(&push, (uint32_t *)push_map, PUSH_SIZE / 4);
   p = &push;

   P_MTHD(p, NV9097, SET_OBJECT);
   P_NV9097_SET_OBJECT(p, {
      .class_id = dev->info.cls_eng3d,
      .engine_id = 0,
   });
}

void
mme_fermi_sim_test::submit_push()
{
   struct drm_nouveau_gem_pushbuf_bo bos[2];
   memset(bos, 0, sizeof(bos));

   bos[0].handle = push_bo->handle,
   bos[0].valid_domains = NOUVEAU_GEM_DOMAIN_GART;
   bos[0].read_domains = NOUVEAU_GEM_DOMAIN_GART;

   bos[1].handle = data_bo->handle,
   bos[1].valid_domains = NOUVEAU_GEM_DOMAIN_GART;
   bos[1].read_domains = NOUVEAU_GEM_DOMAIN_GART;
   bos[1].write_domains = NOUVEAU_GEM_DOMAIN_GART;

   struct drm_nouveau_gem_pushbuf_push push;
   memset(&push, 0, sizeof(push));

   push.bo_index = 0;
   push.offset = 0;
   push.length = nv_push_dw_count(&this->push) * 4;

   struct drm_nouveau_gem_pushbuf req;
   memset(&req, 0, sizeof(req));

   req.channel = ctx->channel;
   req.nr_buffers = 2;
   req.buffers = (uintptr_t)bos;
   req.nr_push = 1;
   req.push = (uintptr_t)&push;

   int ret = drmCommandWriteRead(dev->fd, DRM_NOUVEAU_GEM_PUSHBUF,
                                 &req, sizeof(req));
   ASSERT_EQ(ret, 0);

   bool ok = nouveau_ws_bo_wait(data_bo, NOUVEAU_WS_BO_RDWR);
   ASSERT_TRUE(ok);
}

void
mme_fermi_sim_test::push_macro(uint32_t id, const std::vector<uint32_t> &macro)
{
   P_MTHD(p, NV9097, LOAD_MME_START_ADDRESS_RAM_POINTER);
   P_NV9097_LOAD_MME_START_ADDRESS_RAM_POINTER(p, id);
   P_NV9097_LOAD_MME_START_ADDRESS_RAM(p, 0);
   P_1INC(p, NV9097, LOAD_MME_INSTRUCTION_RAM_POINTER);
   P_NV9097_LOAD_MME_INSTRUCTION_RAM_POINTER(p, 0);
   P_INLINE_ARRAY(p, &macro[0], macro.size());
}

void
mme_fermi_sim_test::test_macro(const mme_builder *b,
                               const std::vector<uint32_t>& macro,
                               const std::vector<uint32_t>& params)
{
   const uint32_t data_dwords = DATA_BO_SIZE / sizeof(uint32_t);

   std::vector<mme_fermi_inst> insts(macro.size());
   mme_fermi_decode(&insts[0], &macro[0], macro.size());

   /* First, make a copy of the data and simulate the macro */
   std::vector<uint32_t> sim_data(data, data + (DATA_BO_SIZE / 4));
   mme_fermi_sim_mem sim_mem = {
      .addr = data_addr,
      .data = &sim_data[0],
      .size = DATA_BO_SIZE,
   };
   mme_fermi_sim(insts.size(), &insts[0],
                 params.size(), &params[0],
                 1, &sim_mem);

   /* Now run the macro on the GPU */
   push_macro(0, macro);

   P_1INC(p, NV9097, CALL_MME_MACRO(0));
   if (params.empty()) {
      P_NV9097_CALL_MME_MACRO(p, 0, 0);
   } else {
      P_INLINE_ARRAY(p, &params[0], params.size());
   }

   submit_push();

   /* Check the results */
   for (uint32_t i = 0; i < data_dwords; i++)
      ASSERT_EQ(data[i], sim_data[i]);
}

static std::vector<uint32_t>
mme_builder_finish_vec(mme_builder *b)
{
   size_t size = 0;
   uint32_t *dw = mme_builder_finish(b, &size);
   std::vector<uint32_t> vec(dw, dw + (size / 4));
   free(dw);
   return vec;
}

static mme_fermi_reg
mme_fermi_value_as_reg(mme_value val)
{
   assert(val.type == MME_VALUE_TYPE_REG);
   return (mme_fermi_reg)(MME_FERMI_REG_ZERO + val.reg);
}

static inline uint32_t
high32(uint64_t x)
{
   return (uint32_t)(x >> 32);
}

static inline uint32_t
low32(uint64_t x)
{
   return (uint32_t)x;
}

static void
mme_store_imm_addr(mme_builder *b, uint64_t addr, mme_value v, bool free_reg)
{
   mme_mthd(b, NV9097_SET_REPORT_SEMAPHORE_A);
   mme_emit(b, mme_imm(high32(addr)));
   mme_emit(b, mme_imm(low32(addr)));
   mme_emit(b, v);
   mme_emit(b, mme_imm(0x10000000));

   if (free_reg && v.type == MME_VALUE_TYPE_REG)
      mme_free_reg(b, v);
}

static void
mme_store(mme_builder *b, struct mme_value64 addr, mme_value v, bool free_reg)
{
   mme_mthd(b, NV9097_SET_REPORT_SEMAPHORE_A);
   mme_emit(b, addr.hi);
   mme_emit(b, addr.lo);
   mme_emit(b, v);
   mme_emit(b, mme_imm(0x10000000));

   if (free_reg && v.type == MME_VALUE_TYPE_REG)
      mme_free_reg(b, v);
}

TEST_F(mme_fermi_sim_test, sanity)
{
   const uint32_t canary = 0xc0ffee01;

   mme_builder b;
   mme_builder_init(&b, &dev->info);

   mme_store_imm_addr(&b, data_addr, mme_imm(canary), false);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   test_macro(&b, macro, params);
}

TEST_F(mme_fermi_sim_test, add)
{
   mme_builder b;
   mme_builder_init(&b, &dev->info);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);
   mme_value sum = mme_add(&b, x, y);
   mme_store_imm_addr(&b, data_addr, sum, true);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(25);
   params.push_back(138);

   test_macro(&b, macro, params);
}

TEST_F(mme_fermi_sim_test, add_imm)
{
   mme_builder b;
   mme_builder_init(&b, &dev->info);

   mme_value x = mme_load(&b);

   mme_value v0 = mme_add(&b, x, mme_imm(0x00000001));
   mme_store_imm_addr(&b, data_addr + 0, v0, true);

   mme_value v1 = mme_add(&b, x, mme_imm(0xffffffff));
   mme_store_imm_addr(&b, data_addr + 4, v1, true);

   mme_value v2 = mme_add(&b, x, mme_imm(0xffff8000));
   mme_store_imm_addr(&b, data_addr + 8, v2, true);

   mme_value v3 = mme_add(&b, mme_imm(0x00000001), x);
   mme_store_imm_addr(&b, data_addr + 12, v3, true);

   mme_value v4 = mme_add(&b, mme_imm(0xffffffff), x);
   mme_store_imm_addr(&b, data_addr + 16, v4, true);

   mme_value v5 = mme_add(&b, mme_imm(0xffff8000), x);
   mme_store_imm_addr(&b, data_addr + 20, v5, true);

   mme_value v6 = mme_add(&b, mme_zero(), mme_imm(0x00000001));
   mme_store_imm_addr(&b, data_addr + 24, v6, true);

   mme_value v7 = mme_add(&b, mme_zero(), mme_imm(0xffffffff));
   mme_store_imm_addr(&b, data_addr + 28, v7, true);

   mme_value v8 = mme_add(&b, mme_zero(), mme_imm(0xffff8000));
   mme_store_imm_addr(&b, data_addr + 32, v8, true);

   auto macro = mme_builder_finish_vec(&b);

   uint32_t vals[] = {
      0x0000ffff,
      0x00008000,
      0x0001ffff,
      0xffffffff,
   };

   for (uint32_t i = 0; i < ARRAY_SIZE(vals); i++) {
      reset_push();

      std::vector<uint32_t> params;
      params.push_back(vals[i]);

      test_macro(&b, macro, params);
   }
}

TEST_F(mme_fermi_sim_test, add_imm_no_carry)
{
   mme_builder b;
   mme_builder_init(&b, &dev->info);

   mme_value x_lo = mme_load(&b);
   mme_value x_hi = mme_load(&b);

   mme_value v1_lo = mme_alloc_reg(&b);
   mme_value v1_hi = mme_alloc_reg(&b);
   mme_fermi_asm(&b, i) {
      i.op = MME_FERMI_OP_ADD_IMM;
      i.assign_op = MME_FERMI_ASSIGN_OP_MOVE;
      i.dst = mme_fermi_value_as_reg(v1_lo);
      i.src[0] = mme_fermi_value_as_reg(x_lo);
      i.imm = 0x0001;
   }

   mme_fermi_asm(&b, i) {
      i.op = MME_FERMI_OP_ADD_IMM;
      i.assign_op = MME_FERMI_ASSIGN_OP_MOVE;
      i.dst = mme_fermi_value_as_reg(v1_hi);
      i.src[0] = mme_fermi_value_as_reg(x_hi);
      i.imm = 0x0000;
   }
   mme_store_imm_addr(&b, data_addr + 0,  v1_lo, true);
   mme_store_imm_addr(&b, data_addr + 4,  v1_hi, true);

   mme_value v2_lo = mme_alloc_reg(&b);
   mme_value v2_hi = mme_alloc_reg(&b);
   mme_fermi_asm(&b, i) {
      i.op = MME_FERMI_OP_ADD_IMM;
      i.assign_op = MME_FERMI_ASSIGN_OP_MOVE;
      i.dst = mme_fermi_value_as_reg(v2_lo);
      i.src[0] = mme_fermi_value_as_reg(x_lo);
      i.imm = 0x0000;
   }

   mme_fermi_asm(&b, i) {
      i.op = MME_FERMI_OP_ADD_IMM;
      i.assign_op = MME_FERMI_ASSIGN_OP_MOVE;
      i.dst = mme_fermi_value_as_reg(v2_hi);
      i.src[0] = mme_fermi_value_as_reg(x_hi);
      i.imm = 0x0001;
   }
   mme_store_imm_addr(&b, data_addr + 8,  v2_lo, true);
   mme_store_imm_addr(&b, data_addr + 12, v2_hi, true);

   mme_value v3_lo = mme_alloc_reg(&b);
   mme_value v3_hi = mme_alloc_reg(&b);
   mme_fermi_asm(&b, i) {
      i.op = MME_FERMI_OP_ADD_IMM;
      i.assign_op = MME_FERMI_ASSIGN_OP_MOVE;
      i.dst = mme_fermi_value_as_reg(v2_lo);
      i.src[0] = mme_fermi_value_as_reg(x_lo);
      i.imm = 0x0000;
   }

   mme_fermi_asm(&b, i) {
      i.op = MME_FERMI_OP_ADD_IMM;
      i.assign_op = MME_FERMI_ASSIGN_OP_MOVE;
      i.dst = mme_fermi_value_as_reg(v2_hi);
      i.src[0] = mme_fermi_value_as_reg(x_hi);
      i.imm = 0xffff;
   }
   mme_store_imm_addr(&b, data_addr + 16, v3_lo, true);
   mme_store_imm_addr(&b, data_addr + 20, v3_hi, true);

   mme_value v4_lo = mme_alloc_reg(&b);
   mme_value v4_hi = mme_alloc_reg(&b);
   mme_fermi_asm(&b, i) {
      i.op = MME_FERMI_OP_ADD_IMM;
      i.assign_op = MME_FERMI_ASSIGN_OP_MOVE;
      i.dst = mme_fermi_value_as_reg(v2_lo);
      i.src[0] = mme_fermi_value_as_reg(x_lo);
      i.imm = 0x0000;
   }

   mme_fermi_asm(&b, i) {
      i.op = MME_FERMI_OP_ADD_IMM;
      i.assign_op = MME_FERMI_ASSIGN_OP_MOVE;
      i.dst = mme_fermi_value_as_reg(v2_hi);
      i.src[0] = mme_fermi_value_as_reg(x_hi);
      i.imm = 0x8000;
   }
   mme_store_imm_addr(&b, data_addr + 24, v4_lo, true);
   mme_store_imm_addr(&b, data_addr + 28, v4_hi, true);

   auto macro = mme_builder_finish_vec(&b);

   uint64_t vals[] = {
      0x0000ffffffffffffull,
      0x0000ffffffff8000ull,
      0x0000ffff00000000ull,
      0x0000800000000000ull,
      0x00008000ffffffffull,
      0x0001ffff00000000ull,
      0xffffffff00000000ull,
      0xffffffffffffffffull,
   };

   for (uint32_t i = 0; i < ARRAY_SIZE(vals); i++) {
      reset_push();

      std::vector<uint32_t> params;
      params.push_back(low32(vals[i]));
      params.push_back(high32(vals[i]));

      test_macro(&b, macro, params);
   }
}

TEST_F(mme_fermi_sim_test, addc)
{
   mme_builder b;
   mme_builder_init(&b, &dev->info);

   struct mme_value64 x = { mme_load(&b), mme_load(&b) };
   struct mme_value64 y = { mme_load(&b), mme_load(&b) };

   struct mme_value64 sum = mme_add64(&b, x, y);

   mme_store_imm_addr(&b, data_addr + 0, sum.lo, true);
   mme_store_imm_addr(&b, data_addr + 4, sum.hi, true);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(0x80008650);
   params.push_back(0x596);
   params.push_back(0x8000a8f6);
   params.push_back(0x836);

   test_macro(&b, macro, params);
}

TEST_F(mme_fermi_sim_test, sub)
{
   mme_builder b;
   mme_builder_init(&b, &dev->info);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);
   mme_value diff = mme_sub(&b, x, y);
   mme_store_imm_addr(&b, data_addr, diff, true);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(25);
   params.push_back(138);

   test_macro(&b, macro, params);
}

TEST_F(mme_fermi_sim_test, subb)
{
   mme_builder b;
   mme_builder_init(&b, &dev->info);

   struct mme_value64 x = { mme_load(&b), mme_load(&b) };
   struct mme_value64 y = { mme_load(&b), mme_load(&b) };

   struct mme_value64 sum = mme_sub64(&b, x, y);

   mme_store_imm_addr(&b, data_addr + 0, sum.lo, true);
   mme_store_imm_addr(&b, data_addr + 4, sum.hi, true);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(0x80008650);
   params.push_back(0x596);
   params.push_back(0x8000a8f6);
   params.push_back(0x836);

   test_macro(&b, macro, params);
}

#define SHIFT_TEST(op)                                                     \
TEST_F(mme_fermi_sim_test, op)                                             \
{                                                                          \
   mme_builder b;                                                          \
   mme_builder_init(&b, &dev->info);                                       \
                                                                           \
   mme_value val = mme_load(&b);                                           \
   mme_value shift1 = mme_load(&b);                                        \
   mme_value shift2 = mme_load(&b);                                        \
   mme_store_imm_addr(&b, data_addr + 0, mme_##op(&b, val, shift1), true); \
   mme_store_imm_addr(&b, data_addr + 4, mme_##op(&b, val, shift2), true); \
                                                                           \
   auto macro = mme_builder_finish_vec(&b);                                \
                                                                           \
   std::vector<uint32_t> params;                                           \
   params.push_back(0x0c406fe0);                                           \
   params.push_back(5);                                                    \
   params.push_back(51);                                                   \
                                                                           \
   test_macro(&b, macro, params);                                          \
}

SHIFT_TEST(sll)
SHIFT_TEST(srl)

#undef SHIFT_TEST

#define BITOP_TEST(op)                                   \
TEST_F(mme_fermi_sim_test, op)                           \
{                                                        \
   mme_builder b;                                        \
   mme_builder_init(&b, &dev->info);                     \
                                                         \
   mme_value x = mme_load(&b);                           \
   mme_value y = mme_load(&b);                           \
   mme_value v1 = mme_##op(&b, x, y);                    \
   mme_value v2 = mme_##op(&b, x, mme_imm(0xffff8000));  \
   mme_value v3 = mme_##op(&b, x, mme_imm(0xffffffff));  \
   mme_store_imm_addr(&b, data_addr + 0, v1, true);      \
   mme_store_imm_addr(&b, data_addr + 4, v2, true);      \
   mme_store_imm_addr(&b, data_addr + 8, v3, true);      \
                                                         \
   auto macro = mme_builder_finish_vec(&b);              \
                                                         \
   std::vector<uint32_t> params;                         \
   params.push_back(0x0c406fe0);                         \
   params.push_back(0x00fff0c0);                         \
                                                         \
   test_macro(&b, macro, params);                        \
}

BITOP_TEST(and)
//BITOP_TEST(and_not)
BITOP_TEST(nand)
BITOP_TEST(or)
BITOP_TEST(xor)

#undef BITOP_TEST

static bool c_ine(int32_t x, int32_t y) { return x != y; };
static bool c_ieq(int32_t x, int32_t y) { return x == y; };

#define IF_TEST(op)                                                  \
TEST_F(mme_fermi_sim_test, if_##op)                                  \
{                                                                    \
   mme_builder b;                                                    \
   mme_builder_init(&b, &dev->info);                                 \
                                                                     \
   mme_value x = mme_load(&b);                                       \
   mme_value y = mme_load(&b);                                       \
   mme_value i = mme_mov(&b, mme_zero());                            \
                                                                     \
   mme_start_if_##op(&b, x, y);                                      \
   {                                                                 \
      mme_add_to(&b, i, i, mme_imm(1));                              \
      mme_add_to(&b, i, i, mme_imm(1));                              \
   }                                                                 \
   mme_end_if(&b);                                                   \
   mme_add_to(&b, i, i, mme_imm(1));                                 \
   mme_add_to(&b, i, i, mme_imm(1));                                 \
   mme_add_to(&b, i, i, mme_imm(1));                                 \
                                                                     \
   mme_store_imm_addr(&b, data_addr + 0, i, true);                   \
                                                                     \
   auto macro = mme_builder_finish_vec(&b);                          \
                                                                     \
   uint32_t vals[] = {23, 56, (uint32_t)-5, (uint32_t)-10, 56, 14};  \
                                                                     \
   for (uint32_t i = 0; i < ARRAY_SIZE(vals) - 1; i++) {             \
      reset_push();                                                  \
                                                                     \
      std::vector<uint32_t> params;                                  \
      params.push_back(vals[i + 0]);                                 \
      params.push_back(vals[i + 1]);                                 \
                                                                     \
      test_macro(&b, macro, params);                                 \
                                                                     \
      ASSERT_EQ(data[0], c_##op(params[0], params[1]) ? 5 : 3);      \
   }                                                                 \
}

IF_TEST(ieq)
IF_TEST(ine)

#undef IF_TEST

static inline void
mme_fermi_inc_whole_inst(mme_builder *b, mme_value val)
{
   mme_fermi_asm(b, i) {
      i.op = MME_FERMI_OP_ADD_IMM;
      i.assign_op = MME_FERMI_ASSIGN_OP_MOVE;
      i.dst = mme_fermi_value_as_reg(val);
      i.src[0] = mme_fermi_value_as_reg(val);
      i.imm = 1;
   }
}

#define WHILE_TEST(op, start, step, bound)               \
TEST_F(mme_fermi_sim_test, while_##op)                   \
{                                                        \
   mme_builder b;                                        \
   mme_builder_init(&b, &dev->info);                     \
                                                         \
   mme_value x = mme_mov(&b, mme_zero());                \
   mme_value y = mme_mov(&b, mme_zero());                \
   mme_value z = mme_mov(&b, mme_imm(start));            \
   mme_value w = mme_mov(&b, mme_zero());                \
   mme_value v = mme_mov(&b, mme_zero());                \
                                                         \
   for (uint32_t j = 0; j < 5; j++)                      \
      mme_fermi_inc_whole_inst(&b, x);                   \
   mme_store_imm_addr(&b, data_addr + 0, x, true);       \
                                                         \
   mme_while(&b, op, z, mme_imm(bound)) {                \
      for (uint32_t j = 0; j < 5; j++)                   \
         mme_fermi_inc_whole_inst(&b, y);                \
                                                         \
      mme_add_to(&b, z, z, mme_imm(step));               \
                                                         \
      for (uint32_t j = 0; j < 5; j++)                   \
         mme_fermi_inc_whole_inst(&b, w);                \
   }                                                     \
   mme_store_imm_addr(&b, data_addr + 4, y, true);       \
   mme_store_imm_addr(&b, data_addr + 8, z, true);       \
   mme_store_imm_addr(&b, data_addr + 12, w, true);      \
                                                         \
   for (uint32_t j = 0; j < 5; j++)                      \
      mme_fermi_inc_whole_inst(&b, v);                   \
                                                         \
   mme_store_imm_addr(&b, data_addr + 16, v, true);      \
                                                         \
   auto macro = mme_builder_finish_vec(&b);              \
                                                         \
   uint32_t end = (uint32_t)(start), count = 0;          \
   while (c_##op(end, (bound))) {                        \
      end += (uint32_t)(step);                           \
      count++;                                           \
   }                                                     \
                                                         \
   std::vector<uint32_t> params;                         \
   test_macro(&b, macro, params);                        \
   ASSERT_EQ(data[0], 5);                                \
   ASSERT_EQ(data[1], 5 * count);                        \
   ASSERT_EQ(data[2], end);                              \
   ASSERT_EQ(data[3], 5 * count);                        \
   ASSERT_EQ(data[4], 5);                                \
}

WHILE_TEST(ieq, 0, 5, 0)
WHILE_TEST(ine, 0, 1, 7)

#undef WHILE_TWST


TEST_F(mme_fermi_sim_test, loop)
{
   mme_builder b;
   mme_builder_init(&b, &dev->info);

   mme_value count = mme_load(&b);

   mme_value x = mme_mov(&b, mme_zero());
   mme_value y = mme_mov(&b, mme_zero());

   mme_loop(&b, count) {
      mme_fermi_asm(&b, i) { } /* noop */
      mme_add_to(&b, x, x, count);
   }
   mme_add_to(&b, y, y, mme_imm(1));
   mme_fermi_asm(&b, i) { } /* noop */
   mme_fermi_asm(&b, i) { } /* noop */
   mme_fermi_asm(&b, i) { } /* noop */

   mme_store_imm_addr(&b, data_addr + 0,  count, true);
   mme_store_imm_addr(&b, data_addr + 4,  x, true);
   mme_store_imm_addr(&b, data_addr + 8,  y, true);

   auto macro = mme_builder_finish_vec(&b);

   uint32_t counts[] = {0, 1, 5, 9};

   for (uint32_t i = 0; i < ARRAY_SIZE(counts); i++) {
      reset_push();

      std::vector<uint32_t> params;
      params.push_back(counts[i]);

      test_macro(&b, macro, params);
      ASSERT_EQ(data[0], counts[i]);
      ASSERT_EQ(data[1], counts[i] * counts[i]);
      ASSERT_EQ(data[2], 1);
   }
}

TEST_F(mme_fermi_sim_test, merge)
{
   mme_builder b;
   mme_builder_init(&b, &dev->info);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);

   mme_value m1 = mme_merge(&b, x, y, 12, 12, 20);
   mme_store_imm_addr(&b, data_addr + 0,  m1, true);

   mme_value m2 = mme_merge(&b, x, y, 12, 8,  20);
   mme_store_imm_addr(&b, data_addr + 4,  m2, true);

   mme_value m3 = mme_merge(&b, x, y, 8,  12, 20);
   mme_store_imm_addr(&b, data_addr + 8,  m3, true);

   mme_value m4 = mme_merge(&b, x, y, 12, 16, 8);
   mme_store_imm_addr(&b, data_addr + 12, m4, true);

   mme_value m5 = mme_merge(&b, x, y, 24, 12, 8);
   mme_store_imm_addr(&b, data_addr + 16, m5, true);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(0x0c406fe0);
   params.push_back(0x76543210u);

   test_macro(&b, macro, params);
}

TEST_F(mme_fermi_sim_test, branch_delay_slot)
{
   mme_builder b;
   mme_builder_init(&b, &dev->info);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);

   mme_fermi_asm(&b, i) {
      i.op = MME_FERMI_OP_BRANCH;
      i.src[0] = MME_FERMI_REG_ZERO;
      i.imm = 2;
      i.branch.no_delay = false;
      i.branch.not_zero = false;
   }

   mme_value res = mme_add(&b, x, y);

   mme_store_imm_addr(&b, data_addr + 0, res, true);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(3);
   params.push_back(1);

   test_macro(&b, macro, params);
   ASSERT_EQ(data[0], 4);
}

TEST_F(mme_fermi_sim_test, state)
{
   mme_builder b;
   mme_builder_init(&b, &dev->info);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);

   mme_mthd(&b, NV9097_SET_MME_SHADOW_SCRATCH(5));
   mme_emit(&b, x);

   mme_mthd(&b, NV9097_SET_MME_SHADOW_SCRATCH(8));
   mme_emit(&b, y);

   mme_value y2 = mme_state(&b, NV9097_SET_MME_SHADOW_SCRATCH(8));
   mme_value x2 = mme_state(&b, NV9097_SET_MME_SHADOW_SCRATCH(5));

   mme_store_imm_addr(&b, data_addr + 0, y2, true);
   mme_store_imm_addr(&b, data_addr + 4, x2, true);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(-10);
   params.push_back(5);

   test_macro(&b, macro, params);
}

TEST_F(mme_fermi_sim_test, scratch_limit)
{
   static const uint32_t chunk_size = 32;

   mme_builder b;
   mme_builder_init(&b, &dev->info);

   mme_value start = mme_load(&b);
   mme_value count = mme_load(&b);

   mme_value i = mme_mov(&b, start);
   mme_loop(&b, count) {
      mme_mthd_arr(&b, NV9097_SET_MME_SHADOW_SCRATCH(0), i);
      mme_emit(&b, i);
      mme_add_to(&b, i, i, mme_imm(1));
   }
   mme_free_reg(&b, i);

   mme_value j = mme_mov(&b, start);
   mme_free_reg(&b, start);
   struct mme_value64 addr = mme_mov64(&b, mme_imm64(data_addr));

   mme_loop(&b, count) {
      mme_value x = mme_state_arr(&b, NV9097_SET_MME_SHADOW_SCRATCH(0), j);
      mme_store(&b, addr, x, true);
      mme_add_to(&b, j, j, mme_imm(1));
      mme_add64_to(&b, addr, addr, mme_imm64(4));
   }
   mme_free_reg(&b, j);
   mme_free_reg(&b, count);

   auto macro = mme_builder_finish_vec(&b);

   for (uint32_t i = 0; i < MME_FERMI_SCRATCH_COUNT; i += chunk_size) {
      reset_push();

      push_macro(0, macro);

      P_1INC(p, NV9097, CALL_MME_MACRO(0));
      P_INLINE_DATA(p, i);
      P_INLINE_DATA(p, chunk_size);

      submit_push();

      for (uint32_t j = 0; j < chunk_size; j++)
         ASSERT_EQ(data[j], i + j);
   }
}

TEST_F(mme_fermi_sim_test, load_imm_to_reg)
{
   mme_builder b;
   mme_builder_init(&b, &dev->info);

   uint32_t vals[] = {
      0x0001ffff,
      0x1ffff000,
      0x0007ffff,
      0x00080000,
      0x7fffffff,
      0x80000000,
      0xffffffff,
   };

   for (uint32_t i = 0; i < ARRAY_SIZE(vals); i++)
      mme_store_imm_addr(&b, data_addr + i * 4, mme_imm(vals[i]), false);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;

   test_macro(&b, macro, params);

   for (uint32_t i = 0; i < ARRAY_SIZE(vals); i++)
      ASSERT_EQ(data[i], vals[i]);
}
