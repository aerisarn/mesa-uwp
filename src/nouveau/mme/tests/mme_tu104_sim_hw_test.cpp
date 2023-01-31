#include <fcntl.h>
#include <string.h>
#include <xf86drm.h>
#include <vector>

#include <gtest/gtest.h>

#include "mme_builder.h"
#include "mme_tu104_sim.h"

#include "nouveau_bo.h"
#include "nouveau_context.h"
#include "nouveau_device.h"

/* nouveau_drm.h isn't C++-friendly */
#define class cls
#include <nouveau_drm.h>
#undef class

#include <xf86drm.h>

#include "nv_push.h"
#include "nvk_clc597.h"

class mme_tu104_sim_test : public ::testing::Test {
public:
   mme_tu104_sim_test();
   ~mme_tu104_sim_test();

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

private:
   struct nouveau_ws_device *dev;
   struct nouveau_ws_context *ctx;
   struct nouveau_ws_bo *data_bo;
   struct nouveau_ws_bo *push_bo;
   void *push_map;
   struct nv_push push;
};

mme_tu104_sim_test::mme_tu104_sim_test() :
  data(NULL), dev(NULL), ctx(NULL), data_bo(NULL), push_bo(NULL)
{
   memset(&push, 0, sizeof(push));
}

mme_tu104_sim_test::~mme_tu104_sim_test()
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
mme_tu104_sim_test::SetUp()
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

         if (dev->info.cls_eng3d < TURING_A) {
            nouveau_ws_device_destroy(dev);
            dev = NULL;
            continue;
         }

         /* Found a Turning+ device */
         break;
      }
   }

   /* We need a Turing+ device */
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
mme_tu104_sim_test::reset_push()
{
   nv_push_init(&push, (uint32_t *)push_map, PUSH_SIZE / 4);
   p = &push;

   P_MTHD(p, NVC597, SET_OBJECT);
   P_NVC597_SET_OBJECT(p, {
      .class_id = dev->info.cls_eng3d,
      .engine_id = 0,
   });
}

void
mme_tu104_sim_test::submit_push()
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
mme_tu104_sim_test::push_macro(uint32_t id, const std::vector<uint32_t> &macro)
{
   P_MTHD(p, NVC597, LOAD_MME_START_ADDRESS_RAM_POINTER);
   P_NVC597_LOAD_MME_START_ADDRESS_RAM_POINTER(p, id);
   P_NVC597_LOAD_MME_START_ADDRESS_RAM(p, 0);
   P_1INC(p, NVC597, LOAD_MME_INSTRUCTION_RAM_POINTER);
   P_NVC597_LOAD_MME_INSTRUCTION_RAM_POINTER(p, 0);
   P_INLINE_ARRAY(p, &macro[0], macro.size());
}

void
mme_tu104_sim_test::test_macro(const mme_builder *b,
                               const std::vector<uint32_t>& macro,
                               const std::vector<uint32_t>& params)
{
   const uint32_t data_dwords = DATA_BO_SIZE / sizeof(uint32_t);

   std::vector<mme_tu104_inst> insts(macro.size() / 3);
   mme_tu104_decode(&insts[0], &macro[0], macro.size() / 3);

   /* First, make a copy of the data and simulate the macro */
   std::vector<uint32_t> sim_data(data, data + (DATA_BO_SIZE / 4));
   mme_tu104_sim_mem sim_mem = {
      .addr = data_addr,
      .data = &sim_data[0],
      .size = DATA_BO_SIZE,
   };
   mme_tu104_sim(insts.size(), &insts[0],
                 params.size(), &params[0],
                 1, &sim_mem);

   /* Now run the macro on the GPU */
   push_macro(0, macro);

   P_1INC(p, NVC597, CALL_MME_MACRO(0));
   if (params.empty()) {
      P_NVC597_CALL_MME_MACRO(p, 0, 0);
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

static mme_tu104_reg
mme_value_as_reg(mme_value val)
{
   assert(val.type == MME_VALUE_TYPE_REG);
   return (mme_tu104_reg)(MME_TU104_REG_R0 + val.reg);
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
mme_store_imm_addr(mme_builder *b, uint64_t addr, mme_value v)
{
   mme_mthd(b, NVC597_SET_REPORT_SEMAPHORE_A);
   mme_emit(b, mme_imm(high32(addr)));
   mme_emit(b, mme_imm(low32(addr)));
   mme_emit(b, v);
   mme_emit(b, mme_imm(0x10000000));
}

static void
mme_store(mme_builder *b, struct mme_value64 addr, mme_value v)
{
   mme_mthd(b, NVC597_SET_REPORT_SEMAPHORE_A);
   mme_emit(b, addr.hi);
   mme_emit(b, addr.lo);
   mme_emit(b, v);
   mme_emit(b, mme_imm(0x10000000));
}

TEST_F(mme_tu104_sim_test, sanity)
{
   const uint32_t canary = 0xc0ffee01;

   mme_builder b;
   mme_builder_init(&b);

   mme_store_imm_addr(&b, data_addr, mme_imm(canary));

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, multi_param)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value v0 = mme_alloc_reg(&b);
   mme_value v1 = mme_alloc_reg(&b);

   mme_tu104_asm(&b, i) {
      i.alu[0].dst = mme_value_as_reg(v0);
      i.alu[0].src[0] = MME_TU104_REG_LOAD1;
      i.alu[1].dst = mme_value_as_reg(v1);
      i.alu[1].src[0] = MME_TU104_REG_LOAD0;
      i.imm[0] = (1<<12) | (NVC597_SET_MME_SHADOW_SCRATCH(12) >> 2);
      i.out[0].mthd = MME_TU104_OUT_OP_IMM0;
      i.out[0].emit = MME_TU104_OUT_OP_LOAD0;
      i.imm[1] = (1<<12) | (NVC597_SET_MME_SHADOW_SCRATCH(35) >> 2);
      i.out[1].mthd = MME_TU104_OUT_OP_IMM1;
      i.out[1].emit = MME_TU104_OUT_OP_LOAD1;
   }

   mme_value v2 = mme_state(&b, NVC597_SET_MME_SHADOW_SCRATCH(12));
   mme_value v3 = mme_state(&b, NVC597_SET_MME_SHADOW_SCRATCH(35));

   mme_store_imm_addr(&b, data_addr + 0, v0);
   mme_store_imm_addr(&b, data_addr + 4, v1);
   mme_store_imm_addr(&b, data_addr + 8, v2);
   mme_store_imm_addr(&b, data_addr + 12, v3);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(2581);
   params.push_back(3048);

   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, pred_param)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value v0 = mme_load(&b);
   mme_value v1 = mme_mov(&b, mme_imm(240));

   mme_tu104_asm(&b, i) {
      i.pred_mode = MME_TU104_PRED_TTTT;
      i.alu[0].dst = mme_value_as_reg(v1);
      i.alu[0].src[0] = MME_TU104_REG_LOAD0;
   }

   mme_value v2 = mme_load(&b);

   mme_store_imm_addr(&b, data_addr + 0, v0);
   mme_store_imm_addr(&b, data_addr + 4, v1);
   mme_store_imm_addr(&b, data_addr + 8, v2);

   auto macro = mme_builder_finish_vec(&b);

   for (uint32_t j = 0; j < 4; j++) {
      reset_push();

      std::vector<uint32_t> params;
      params.push_back((j & 1) * 2043);
      params.push_back((j & 2) * 523);
      params.push_back(2581);
      params.push_back(3048);

      test_macro(&b, macro, params);
   }
}

TEST_F(mme_tu104_sim_test, out_imm0)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_mthd(&b, NVC597_SET_REPORT_SEMAPHORE_A);
   mme_emit(&b, mme_imm(high32(data_addr + 0)));
   mme_emit(&b, mme_imm(low32(data_addr + 0)));
   mme_tu104_asm(&b, i) {
      i.imm[0] = 0x1234;
      i.out[0].emit = MME_TU104_OUT_OP_IMM0;
   }
   mme_emit(&b, mme_imm(0x10000000));

   mme_mthd(&b, NVC597_SET_REPORT_SEMAPHORE_A);
   mme_emit(&b, mme_imm(high32(data_addr + 4)));
   mme_emit(&b, mme_imm(low32(data_addr + 4)));
   mme_tu104_asm(&b, i) {
      i.imm[0] = 0x8765;
      i.out[0].emit = MME_TU104_OUT_OP_IMM0;
   }
   mme_emit(&b, mme_imm(0x10000000));

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, out_imm1)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_mthd(&b, NVC597_SET_REPORT_SEMAPHORE_A);
   mme_emit(&b, mme_imm(high32(data_addr + 0)));
   mme_emit(&b, mme_imm(low32(data_addr + 0)));
   mme_tu104_asm(&b, i) {
      i.imm[1] = 0x1234;
      i.out[0].emit = MME_TU104_OUT_OP_IMM1;
   }
   mme_emit(&b, mme_imm(0x10000000));

   mme_mthd(&b, NVC597_SET_REPORT_SEMAPHORE_A);
   mme_emit(&b, mme_imm(high32(data_addr + 4)));
   mme_emit(&b, mme_imm(low32(data_addr + 4)));
   mme_tu104_asm(&b, i) {
      i.imm[1] = 0x8765;
      i.out[0].emit = MME_TU104_OUT_OP_IMM1;
   }
   mme_emit(&b, mme_imm(0x10000000));

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, out_immhigh0)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_mthd(&b, NVC597_SET_REPORT_SEMAPHORE_A);
   mme_emit(&b, mme_imm(high32(data_addr + 0)));
   mme_emit(&b, mme_imm(low32(data_addr + 0)));
   mme_tu104_asm(&b, i) {
      i.imm[0] = 0x1234;
      i.out[0].emit = MME_TU104_OUT_OP_IMMHIGH0;
   }
   mme_emit(&b, mme_imm(0x10000000));

   mme_mthd(&b, NVC597_SET_REPORT_SEMAPHORE_A);
   mme_emit(&b, mme_imm(high32(data_addr + 4)));
   mme_emit(&b, mme_imm(low32(data_addr + 4)));
   mme_tu104_asm(&b, i) {
      i.imm[0] = 0x8765;
      i.out[1].emit = MME_TU104_OUT_OP_IMMHIGH0;
   }
   mme_emit(&b, mme_imm(0x10000000));

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, out_immhigh1)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_mthd(&b, NVC597_SET_REPORT_SEMAPHORE_A);
   mme_emit(&b, mme_imm(high32(data_addr + 0)));
   mme_emit(&b, mme_imm(low32(data_addr + 0)));
   mme_tu104_asm(&b, i) {
      i.imm[1] = 0x1234;
      i.out[0].emit = MME_TU104_OUT_OP_IMMHIGH1;
   }
   mme_emit(&b, mme_imm(0x10000000));

   mme_mthd(&b, NVC597_SET_REPORT_SEMAPHORE_A);
   mme_emit(&b, mme_imm(high32(data_addr + 4)));
   mme_emit(&b, mme_imm(low32(data_addr + 4)));
   mme_tu104_asm(&b, i) {
      i.imm[1] = 0x8765;
      i.out[1].emit = MME_TU104_OUT_OP_IMMHIGH1;
   }
   mme_emit(&b, mme_imm(0x10000000));

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, out_imm32)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_mthd(&b, NVC597_SET_REPORT_SEMAPHORE_A);
   mme_emit(&b, mme_imm(high32(data_addr + 0)));
   mme_emit(&b, mme_imm(low32(data_addr + 0)));
   mme_tu104_asm(&b, i) {
      i.imm[0] = 0x1234;
      i.imm[1] = 0x7654;
      i.out[0].emit = MME_TU104_OUT_OP_IMM32;
   }
   mme_emit(&b, mme_imm(0x10000000));

   mme_mthd(&b, NVC597_SET_REPORT_SEMAPHORE_A);
   mme_emit(&b, mme_imm(high32(data_addr + 4)));
   mme_emit(&b, mme_imm(low32(data_addr + 4)));
   mme_tu104_asm(&b, i) {
      i.imm[0] = 0x1234;
      i.imm[1] = 0x7654;
      i.out[1].emit = MME_TU104_OUT_OP_IMM32;
   }
   mme_emit(&b, mme_imm(0x10000000));

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, reg_imm32)
{
   const uint32_t canary = 0xc0ffee01;

   mme_builder b;
   mme_builder_init(&b);

   mme_value v = mme_alloc_reg(&b);

   mme_tu104_asm(&b, i) {
      i.alu[0].dst = mme_value_as_reg(v);
      i.alu[0].op = MME_TU104_ALU_OP_ADD;
      i.alu[0].src[0] = MME_TU104_REG_IMM32,
      i.imm[0] = (uint16_t)canary;
      i.imm[1] = (uint16_t)(canary >> 16);
   }

   mme_store_imm_addr(&b, data_addr, v);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, pred_alu)
{
   static const enum mme_tu104_pred preds[] = {
      MME_TU104_PRED_UUUU,
      MME_TU104_PRED_TTTT,
      MME_TU104_PRED_FFFF,
      MME_TU104_PRED_TTUU,
      MME_TU104_PRED_FFUU,
      MME_TU104_PRED_TFUU,
      MME_TU104_PRED_TUUU,
      MME_TU104_PRED_FUUU,
      MME_TU104_PRED_UUTT,
      MME_TU104_PRED_UUTF,
      MME_TU104_PRED_UUTU,
      MME_TU104_PRED_UUFT,
      MME_TU104_PRED_UUFF,
      MME_TU104_PRED_UUFU,
      MME_TU104_PRED_UUUT,
      MME_TU104_PRED_UUUF,
   };

   for (uint32_t i = 0; i < ARRAY_SIZE(preds); i++) {
      mme_builder b;
      mme_builder_init(&b);

      mme_value pred = mme_load(&b);
      mme_value v0 = mme_mov(&b, mme_imm(i * 100 + 13));
      mme_value v1 = mme_mov(&b, mme_imm(i * 100 + 62));

      mme_tu104_asm(&b, inst) {
         inst.pred = mme_value_as_reg(pred);
         inst.pred_mode = preds[i];
         inst.alu[0].dst = mme_value_as_reg(v0);
         inst.alu[0].src[0] = MME_TU104_REG_IMM;
         inst.imm[0] = i * 100 + 25;
         inst.alu[1].dst = mme_value_as_reg(v1);
         inst.alu[1].src[0] = MME_TU104_REG_IMM;
         inst.imm[1] = i * 100 + 73;
      }

      mme_store_imm_addr(&b, data_addr + i * 8 + 0, v0);
      mme_store_imm_addr(&b, data_addr + i * 8 + 4, v1);

      auto macro = mme_builder_finish_vec(&b);

      for (uint32_t j = 0; j < 2; j++) {
         reset_push();

         std::vector<uint32_t> params;
         params.push_back(j * 25894);

         test_macro(&b, macro, params);
      }
   }
}

TEST_F(mme_tu104_sim_test, pred_out)
{
   static const enum mme_tu104_pred preds[] = {
      MME_TU104_PRED_UUUU,
      MME_TU104_PRED_TTTT,
      MME_TU104_PRED_FFFF,
      MME_TU104_PRED_TTUU,
      MME_TU104_PRED_FFUU,
      MME_TU104_PRED_TFUU,
      MME_TU104_PRED_TUUU,
      MME_TU104_PRED_FUUU,
      MME_TU104_PRED_UUTT,
      MME_TU104_PRED_UUTF,
      MME_TU104_PRED_UUTU,
      MME_TU104_PRED_UUFT,
      MME_TU104_PRED_UUFF,
      MME_TU104_PRED_UUFU,
      MME_TU104_PRED_UUUT,
      MME_TU104_PRED_UUUF,
   };

   for (uint32_t i = 0; i < ARRAY_SIZE(preds); i++) {
      mme_builder b;
      mme_builder_init(&b);

      mme_value pred = mme_load(&b);

      mme_tu104_asm(&b, inst) {
         inst.imm[0] = (1<<12) | (NVC597_SET_MME_SHADOW_SCRATCH(i*2 + 0) >> 2);
         inst.imm[1] = i * 100 + 25;
         inst.out[0].mthd = MME_TU104_OUT_OP_IMM0;
         inst.out[0].emit = MME_TU104_OUT_OP_IMM1;
      }

      mme_tu104_asm(&b, inst) {
         inst.imm[0] = (1<<12) | (NVC597_SET_MME_SHADOW_SCRATCH(i*2 + 1) >> 2);
         inst.imm[1] = i * 100 + 75;
         inst.out[0].mthd = MME_TU104_OUT_OP_IMM0;
         inst.out[0].emit = MME_TU104_OUT_OP_IMM1;
      }

      mme_tu104_asm(&b, inst) {
         inst.pred = mme_value_as_reg(pred);
         inst.pred_mode = preds[i];
         inst.imm[0] = (1<<12) | (NVC597_SET_MME_SHADOW_SCRATCH(i*2 + 0) >> 2);
         inst.imm[1] = (1<<12) | (NVC597_SET_MME_SHADOW_SCRATCH(i*2 + 1) >> 2);
         inst.out[0].mthd = MME_TU104_OUT_OP_IMM0;
         inst.out[0].emit = MME_TU104_OUT_OP_IMM1;
         inst.out[1].mthd = MME_TU104_OUT_OP_IMM1;
         inst.out[1].emit = MME_TU104_OUT_OP_IMM0;
      }

      mme_value v0 = mme_state(&b, NVC597_SET_MME_SHADOW_SCRATCH(i*2 + 0));
      mme_value v1 = mme_state(&b, NVC597_SET_MME_SHADOW_SCRATCH(i*2 + 1));

      mme_store_imm_addr(&b, data_addr + i * 8 + 0, v0);
      mme_store_imm_addr(&b, data_addr + i * 8 + 4, v1);

      auto macro = mme_builder_finish_vec(&b);

      for (uint32_t j = 0; j < 2; j++) {
         reset_push();

         std::vector<uint32_t> params;
         params.push_back(j * 25894);

         test_macro(&b, macro, params);
      }
   }
}

TEST_F(mme_tu104_sim_test, add)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);
   mme_value sum = mme_add(&b, x, y);
   mme_store_imm_addr(&b, data_addr, sum);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(25);
   params.push_back(138);

   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, add_imm)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value x = mme_load(&b);

   mme_value v0 = mme_add(&b, x, mme_imm(0x00000001));
   mme_value v1 = mme_add(&b, x, mme_imm(0xffffffff));
   mme_value v2 = mme_add(&b, x, mme_imm(0xffff8000));
   mme_value v3 = mme_add(&b, mme_imm(0x00000001), x);
   mme_value v4 = mme_add(&b, mme_imm(0xffffffff), x);
   mme_value v5 = mme_add(&b, mme_imm(0xffff8000), x);
   mme_value v6 = mme_add(&b, mme_zero(), mme_imm(0x00000001));
   mme_value v7 = mme_add(&b, mme_zero(), mme_imm(0xffffffff));
   mme_value v8 = mme_add(&b, mme_zero(), mme_imm(0xffff8000));

   mme_store_imm_addr(&b, data_addr + 0,  v0);
   mme_store_imm_addr(&b, data_addr + 4,  v1);
   mme_store_imm_addr(&b, data_addr + 8,  v2);
   mme_store_imm_addr(&b, data_addr + 12, v3);
   mme_store_imm_addr(&b, data_addr + 16, v4);
   mme_store_imm_addr(&b, data_addr + 20, v5);
   mme_store_imm_addr(&b, data_addr + 24, v6);
   mme_store_imm_addr(&b, data_addr + 28, v7);
   mme_store_imm_addr(&b, data_addr + 32, v8);

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

TEST_F(mme_tu104_sim_test, addc)
{
   mme_builder b;
   mme_builder_init(&b);

   struct mme_value64 x = { mme_load(&b), mme_load(&b) };
   struct mme_value64 y = { mme_load(&b), mme_load(&b) };

   struct mme_value64 sum = mme_add64(&b, x, y);

   mme_store_imm_addr(&b, data_addr + 0, sum.lo);
   mme_store_imm_addr(&b, data_addr + 4, sum.hi);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(0x80008650);
   params.push_back(0x596);
   params.push_back(0x8000a8f6);
   params.push_back(0x836);

   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, addc_imm)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value x_lo = mme_load(&b);
   mme_value x_hi = mme_load(&b);

   mme_value v1_lo = mme_alloc_reg(&b);
   mme_value v1_hi = mme_alloc_reg(&b);
   mme_tu104_asm(&b, i) {
      i.alu[0].dst = mme_value_as_reg(v1_lo);
      i.alu[0].op = MME_TU104_ALU_OP_ADD;
      i.alu[0].src[0] = mme_value_as_reg(x_lo);
      i.alu[0].src[1] = MME_TU104_REG_IMM;
      i.imm[0] = 0x0001;
      i.alu[1].dst = mme_value_as_reg(v1_hi);
      i.alu[1].op = MME_TU104_ALU_OP_ADDC;
      i.alu[1].src[0] = mme_value_as_reg(x_hi);
      i.alu[1].src[1] = MME_TU104_REG_IMM;
      i.imm[1] = 0x0000;
   }

   mme_value v2_lo = mme_alloc_reg(&b);
   mme_value v2_hi = mme_alloc_reg(&b);
   mme_tu104_asm(&b, i) {
      i.alu[0].dst = mme_value_as_reg(v2_lo);
      i.alu[0].op = MME_TU104_ALU_OP_ADD;
      i.alu[0].src[0] = mme_value_as_reg(x_lo);
      i.alu[0].src[1] = MME_TU104_REG_IMM;
      i.imm[0] = 0x0000;
      i.alu[1].dst = mme_value_as_reg(v2_hi);
      i.alu[1].op = MME_TU104_ALU_OP_ADDC;
      i.alu[1].src[0] = mme_value_as_reg(x_hi);
      i.alu[1].src[1] = MME_TU104_REG_IMM;
      i.imm[1] = 0x0001;
   }

   mme_value v3_lo = mme_alloc_reg(&b);
   mme_value v3_hi = mme_alloc_reg(&b);
   mme_tu104_asm(&b, i) {
      i.alu[0].dst = mme_value_as_reg(v3_lo);
      i.alu[0].op = MME_TU104_ALU_OP_ADD;
      i.alu[0].src[0] = mme_value_as_reg(x_lo);
      i.alu[0].src[1] = MME_TU104_REG_IMM;
      i.imm[0] = 0x0000;
      i.alu[1].dst = mme_value_as_reg(v3_hi);
      i.alu[1].op = MME_TU104_ALU_OP_ADDC;
      i.alu[1].src[0] = mme_value_as_reg(x_hi);
      i.alu[1].src[1] = MME_TU104_REG_IMM;
      i.imm[1] = 0xffff;
   }

   mme_value v4_lo = mme_alloc_reg(&b);
   mme_value v4_hi = mme_alloc_reg(&b);
   mme_tu104_asm(&b, i) {
      i.alu[0].dst = mme_value_as_reg(v4_lo);
      i.alu[0].op = MME_TU104_ALU_OP_ADD;
      i.alu[0].src[0] = mme_value_as_reg(x_lo);
      i.alu[0].src[1] = MME_TU104_REG_IMM;
      i.imm[0] = 0x0000;
      i.alu[1].dst = mme_value_as_reg(v4_hi);
      i.alu[1].op = MME_TU104_ALU_OP_ADDC;
      i.alu[1].src[0] = mme_value_as_reg(x_hi);
      i.alu[1].src[1] = MME_TU104_REG_IMM;
      i.imm[1] = 0x8000;
   }

   mme_store_imm_addr(&b, data_addr + 0,  v1_lo);
   mme_store_imm_addr(&b, data_addr + 4,  v1_hi);
   mme_store_imm_addr(&b, data_addr + 8,  v2_lo);
   mme_store_imm_addr(&b, data_addr + 12, v2_hi);
   mme_store_imm_addr(&b, data_addr + 16, v3_lo);
   mme_store_imm_addr(&b, data_addr + 20, v3_hi);
   mme_store_imm_addr(&b, data_addr + 24, v4_lo);
   mme_store_imm_addr(&b, data_addr + 28, v4_hi);

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

TEST_F(mme_tu104_sim_test, sub)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);
   mme_value diff = mme_sub(&b, x, y);
   mme_store_imm_addr(&b, data_addr, diff);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(25);
   params.push_back(138);

   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, subb)
{
   mme_builder b;
   mme_builder_init(&b);

   struct mme_value64 x = { mme_load(&b), mme_load(&b) };
   struct mme_value64 y = { mme_load(&b), mme_load(&b) };

   struct mme_value64 diff = mme_sub64(&b, x, y);

   mme_store_imm_addr(&b, data_addr + 0, diff.lo);
   mme_store_imm_addr(&b, data_addr + 4, diff.hi);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(0x80008650);
   params.push_back(0x596);
   params.push_back(0x8000a8f6);
   params.push_back(0x836);

   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, mul)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);
   mme_value sum = mme_mul(&b, x, y);
   mme_store_imm_addr(&b, data_addr, sum);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(25);
   params.push_back(138);

   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, mul_imm)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value x = mme_load(&b);

   mme_value v0 = mme_mul(&b, x, mme_imm(0x00000001));
   mme_value v1 = mme_mul(&b, x, mme_imm(0xffffffff));
   mme_value v2 = mme_mul(&b, x, mme_imm(0xffff8000));
   mme_value v3 = mme_mul(&b, mme_imm(0x00000001), x);
   mme_value v4 = mme_mul(&b, mme_imm(0xffffffff), x);
   mme_value v5 = mme_mul(&b, mme_imm(0xffff8000), x);

   mme_store_imm_addr(&b, data_addr + 0,  v0);
   mme_store_imm_addr(&b, data_addr + 4,  v1);
   mme_store_imm_addr(&b, data_addr + 8,  v2);
   mme_store_imm_addr(&b, data_addr + 12, v3);
   mme_store_imm_addr(&b, data_addr + 16, v4);
   mme_store_imm_addr(&b, data_addr + 20, v5);

   auto macro = mme_builder_finish_vec(&b);

   int32_t vals[] = { 1, -5, -1, 5 };

   for (uint32_t i = 0; i < ARRAY_SIZE(vals); i++) {
      reset_push();

      std::vector<uint32_t> params;
      params.push_back(vals[i]);

      test_macro(&b, macro, params);
   }
}

TEST_F(mme_tu104_sim_test, mul_mulh)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);

   struct mme_value64 prod = mme_imul_32x32_64(&b, x, y);

   mme_store_imm_addr(&b, data_addr + 0, prod.lo);
   mme_store_imm_addr(&b, data_addr + 4, prod.hi);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(0x80008650);
   params.push_back(0x596);

   test_macro(&b, macro, params);
}

static inline struct mme_value
mme_mulu(struct mme_builder *b, struct mme_value x, struct mme_value y)
{
   return mme_tu104_alu(b, MME_TU104_ALU_OP_MULU, x, y, 0);
}

TEST_F(mme_tu104_sim_test, mulu_imm)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value x = mme_load(&b);

   mme_value v0 = mme_mulu(&b, x, mme_imm(0x00000001));
   mme_value v1 = mme_mulu(&b, x, mme_imm(0xffffffff));
   mme_value v2 = mme_mulu(&b, x, mme_imm(0xffff8000));
   mme_value v3 = mme_mulu(&b, mme_imm(0x00000001), x);
   mme_value v4 = mme_mulu(&b, mme_imm(0xffffffff), x);
   mme_value v5 = mme_mulu(&b, mme_imm(0xffff8000), x);

   mme_store_imm_addr(&b, data_addr + 0,  v0);
   mme_store_imm_addr(&b, data_addr + 4,  v1);
   mme_store_imm_addr(&b, data_addr + 8,  v2);
   mme_store_imm_addr(&b, data_addr + 12, v3);
   mme_store_imm_addr(&b, data_addr + 16, v4);
   mme_store_imm_addr(&b, data_addr + 20, v5);

   auto macro = mme_builder_finish_vec(&b);

   int32_t vals[] = { 1, -5, -1, 5 };

   for (uint32_t i = 0; i < ARRAY_SIZE(vals); i++) {
      reset_push();

      std::vector<uint32_t> params;
      params.push_back(vals[i]);

      test_macro(&b, macro, params);
   }
}

TEST_F(mme_tu104_sim_test, mulu_mulh)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);

   struct mme_value64 prod = mme_umul_32x32_64(&b, x, y);

   mme_store_imm_addr(&b, data_addr + 0, prod.lo);
   mme_store_imm_addr(&b, data_addr + 4, prod.hi);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(0x80008650);
   params.push_back(0x596);

   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, clz)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value bits = mme_clz(&b, mme_load(&b));
   mme_store_imm_addr(&b, data_addr, bits);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(0x00406fe0);

   test_macro(&b, macro, params);
}

#define SHIFT_TEST(op)                                               \
TEST_F(mme_tu104_sim_test, op)                                       \
{                                                                    \
   mme_builder b;                                                    \
   mme_builder_init(&b);                                             \
                                                                     \
   mme_value val = mme_load(&b);                                     \
   mme_value shift1 = mme_load(&b);                                  \
   mme_value shift2 = mme_load(&b);                                  \
   mme_store_imm_addr(&b, data_addr + 0, mme_##op(&b, val, shift1)); \
   mme_store_imm_addr(&b, data_addr + 4, mme_##op(&b, val, shift2)); \
                                                                     \
   auto macro = mme_builder_finish_vec(&b);                          \
                                                                     \
   std::vector<uint32_t> params;                                     \
   params.push_back(0x0c406fe0);                                     \
   params.push_back(5);                                              \
   params.push_back(51);                                             \
                                                                     \
   test_macro(&b, macro, params);                                    \
}

SHIFT_TEST(sll)
SHIFT_TEST(srl)
SHIFT_TEST(sra)

#undef SHIFT_TEST

#define BITOP_TEST(op)                                               \
TEST_F(mme_tu104_sim_test, op)                                       \
{                                                                    \
   mme_builder b;                                                    \
   mme_builder_init(&b);                                             \
                                                                     \
   mme_value x = mme_load(&b);                                       \
   mme_value y = mme_load(&b);                                       \
   mme_value v1 = mme_##op(&b, x, y);                                \
   mme_value v2 = mme_##op(&b, x, mme_imm(0xffff8000));              \
   mme_value v3 = mme_##op(&b, x, mme_imm(0xffffffff));              \
   mme_store_imm_addr(&b, data_addr + 0, v1);                        \
   mme_store_imm_addr(&b, data_addr + 4, v2);                        \
   mme_store_imm_addr(&b, data_addr + 8, v3);                        \
                                                                     \
   auto macro = mme_builder_finish_vec(&b);                          \
                                                                     \
   std::vector<uint32_t> params;                                     \
   params.push_back(0x0c406fe0);                                     \
   params.push_back(0x00fff0c0);                                     \
                                                                     \
   test_macro(&b, macro, params);                                    \
}

BITOP_TEST(and)
BITOP_TEST(nand)
BITOP_TEST(or)
BITOP_TEST(xor)

#undef BITOP_TEST

TEST_F(mme_tu104_sim_test, merge)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);

   mme_value m1 = mme_merge(&b, x, y, 12, 12, 20);
   mme_value m2 = mme_merge(&b, x, y, 12, 8,  20);
   mme_value m3 = mme_merge(&b, x, y, 8,  12, 20);
   mme_value m4 = mme_merge(&b, x, y, 12, 16, 8);
   mme_value m5 = mme_merge(&b, x, y, 24, 12, 8);

   mme_store_imm_addr(&b, data_addr + 0,  m1);
   mme_store_imm_addr(&b, data_addr + 4,  m2);
   mme_store_imm_addr(&b, data_addr + 8,  m3);
   mme_store_imm_addr(&b, data_addr + 12, m4);
   mme_store_imm_addr(&b, data_addr + 16, m5);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(0x0c406fe0);
   params.push_back(0x76543210u);

   test_macro(&b, macro, params);
}

#define COMPARISON_TEST(op)                     \
TEST_F(mme_tu104_sim_test, op)                  \
{                                               \
   mme_builder b;                               \
   mme_builder_init(&b);                        \
                                                \
   mme_value x = mme_load(&b);                  \
   mme_value y = mme_load(&b);                  \
   mme_value z = mme_load(&b);                  \
   mme_value w = mme_load(&b);                  \
                                                \
   mme_value v1 = mme_##op(&b, x, y);           \
   mme_value v2 = mme_##op(&b, y, x);           \
   mme_value v3 = mme_##op(&b, y, z);           \
   mme_value v4 = mme_##op(&b, z, y);           \
   mme_value v5 = mme_##op(&b, w, z);           \
   mme_value v6 = mme_##op(&b, z, w);           \
   mme_value v7 = mme_##op(&b, w, w);           \
                                                \
   mme_store_imm_addr(&b, data_addr + 0,  v1);  \
   mme_store_imm_addr(&b, data_addr + 4,  v2);  \
   mme_store_imm_addr(&b, data_addr + 8,  v3);  \
   mme_store_imm_addr(&b, data_addr + 12, v4);  \
   mme_store_imm_addr(&b, data_addr + 16, v5);  \
   mme_store_imm_addr(&b, data_addr + 20, v6);  \
   mme_store_imm_addr(&b, data_addr + 24, v7);  \
                                                \
   auto macro = mme_builder_finish_vec(&b);     \
                                                \
   std::vector<uint32_t> params;                \
   params.push_back(-5);                        \
   params.push_back(-10);                       \
   params.push_back(5);                         \
   params.push_back(10);                        \
                                                \
   test_macro(&b, macro, params);               \
}

COMPARISON_TEST(slt)
COMPARISON_TEST(sltu)
COMPARISON_TEST(sle)
COMPARISON_TEST(sleu)
COMPARISON_TEST(seq)

#undef COMPARISON_TEST

static inline void
mme_inc_whole_inst(mme_builder *b, mme_value val)
{
   mme_tu104_asm(b, i) {
      i.alu[0].dst = mme_value_as_reg(val);
      i.alu[0].op = MME_TU104_ALU_OP_ADD;
      i.alu[0].src[0] = mme_value_as_reg(val);
      i.alu[0].src[1] = MME_TU104_REG_IMM;
      i.imm[0] = 1;
   }
}

TEST_F(mme_tu104_sim_test, loop)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value count = mme_load(&b);

   mme_value x = mme_mov(&b, mme_zero());
   mme_value y = mme_mov(&b, mme_zero());

   mme_loop(&b, count) {
      mme_tu104_asm(&b, i) { } /* noop */
      mme_add_to(&b, x, x, count);
   }
   mme_add_to(&b, y, y, mme_imm(1));
   mme_tu104_asm(&b, i) { } /* noop */
   mme_tu104_asm(&b, i) { } /* noop */
   mme_tu104_asm(&b, i) { } /* noop */

   mme_store_imm_addr(&b, data_addr + 0,  count);
   mme_store_imm_addr(&b, data_addr + 4,  x);
   mme_store_imm_addr(&b, data_addr + 8,  y);

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

TEST_F(mme_tu104_sim_test, jal)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value x = mme_mov(&b, mme_zero());
   mme_value y = mme_mov(&b, mme_zero());

   mme_tu104_asm(&b, i) {
      i.alu[0].op = MME_TU104_ALU_OP_JAL;
      i.imm[0] = (1 << 15) | 6;
   }

   for (uint32_t j = 0; j < 10; j++)
      mme_inc_whole_inst(&b, x);

//   mme_tu104_asm(&b, i) {
//      i.alu[0].op = MME_TU104_ALU_OP_JAL;
//      i.imm[0] = 6;
//   }
//
//   for (uint32_t j = 0; j < 10; j++)
//      mme_inc_whole_inst(&b, y);

   mme_store_imm_addr(&b, data_addr + 0, x);
   mme_store_imm_addr(&b, data_addr + 4, y);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   test_macro(&b, macro, params);
   ASSERT_EQ(data[0], 5);
}

TEST_F(mme_tu104_sim_test, bxx_fwd)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value vals[10];
   for (uint32_t i = 0; i < 10; i++)
      vals[i] = mme_mov(&b, mme_zero());

   mme_tu104_asm(&b, i) {
      i.alu[0].op = MME_TU104_ALU_OP_BEQ;
      i.imm[0] = (1 << 15) | 6;
   }

   for (uint32_t j = 0; j < 10; j++)
      mme_inc_whole_inst(&b, vals[j]);

   for (uint32_t j = 0; j < 10; j++)
      mme_store_imm_addr(&b, data_addr + j * 4, vals[j]);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, bxx_bwd)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value vals[15];
   for (uint32_t i = 0; i < 15; i++)
      vals[i] = mme_mov(&b, mme_zero());

   mme_tu104_asm(&b, i) {
      i.alu[0].op = MME_TU104_ALU_OP_JAL;
      i.imm[0] = (1 << 15) | 12;
   }

   for (uint32_t j = 0; j < 10; j++)
      mme_inc_whole_inst(&b, vals[j]);

   mme_tu104_asm(&b, i) {
      i.alu[0].op = MME_TU104_ALU_OP_JAL;
      i.imm[0] = (1 << 15) | 2;
   }

   mme_tu104_asm(&b, i) {
      i.alu[0].op = MME_TU104_ALU_OP_BEQ;
      i.imm[0] = (1 << 15) | ((-8) & 0x1fff);
   }

   for (uint32_t j = 10; j < 15; j++)
      mme_inc_whole_inst(&b, vals[j]);

   for (uint32_t j = 0; j < 15; j++)
      mme_store_imm_addr(&b, data_addr + j * 4, vals[j]);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   test_macro(&b, macro, params);
   for (uint32_t j = 0; j < 3; j++)
      ASSERT_EQ(data[j], 0);
   for (uint32_t j = 3; j < 15; j++)
      ASSERT_EQ(data[j], 1);
}

static bool c_ilt(int32_t x, int32_t y) { return x < y; };
static bool c_ult(uint32_t x, uint32_t y) { return x < y; };
static bool c_ile(int32_t x, int32_t y) { return x <= y; };
static bool c_ule(uint32_t x, uint32_t y) { return x <= y; };
static bool c_ieq(int32_t x, int32_t y) { return x == y; };
static bool c_ige(int32_t x, int32_t y) { return x >= y; };
static bool c_uge(uint32_t x, uint32_t y) { return x >= y; };
static bool c_igt(int32_t x, int32_t y) { return x > y; };
static bool c_ugt(uint32_t x, uint32_t y) { return x > y; };
static bool c_ine(int32_t x, int32_t y) { return x != y; };

#define IF_TEST(op)                                                  \
TEST_F(mme_tu104_sim_test, if_##op)                                  \
{                                                                    \
   mme_builder b;                                                    \
   mme_builder_init(&b);                                             \
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
   mme_store_imm_addr(&b, data_addr + 0, i);                         \
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

IF_TEST(ilt)
IF_TEST(ult)
IF_TEST(ile)
IF_TEST(ule)
IF_TEST(ieq)
IF_TEST(ige)
IF_TEST(uge)
IF_TEST(igt)
IF_TEST(ugt)
IF_TEST(ine)

#undef IF_TEST

#define WHILE_TEST(op, start, step, bound)            \
TEST_F(mme_tu104_sim_test, while_##op)                \
{                                                     \
   mme_builder b;                                     \
   mme_builder_init(&b);                              \
                                                      \
   mme_value x = mme_mov(&b, mme_zero());             \
   mme_value y = mme_mov(&b, mme_zero());             \
   mme_value z = mme_mov(&b, mme_imm(start));         \
   mme_value w = mme_mov(&b, mme_zero());             \
   mme_value v = mme_mov(&b, mme_zero());             \
                                                      \
   for (uint32_t j = 0; j < 5; j++)                   \
      mme_inc_whole_inst(&b, x);                      \
                                                      \
   mme_while(&b, op, z, mme_imm(bound)) {             \
      for (uint32_t j = 0; j < 5; j++)                \
         mme_inc_whole_inst(&b, y);                   \
                                                      \
      mme_add_to(&b, z, z, mme_imm(step));            \
                                                      \
      for (uint32_t j = 0; j < 5; j++)                \
         mme_inc_whole_inst(&b, w);                   \
   }                                                  \
                                                      \
   for (uint32_t j = 0; j < 5; j++)                   \
      mme_inc_whole_inst(&b, v);                      \
                                                      \
   mme_store_imm_addr(&b, data_addr + 0, x);          \
   mme_store_imm_addr(&b, data_addr + 4, y);          \
   mme_store_imm_addr(&b, data_addr + 8, z);          \
   mme_store_imm_addr(&b, data_addr + 12, w);         \
   mme_store_imm_addr(&b, data_addr + 16, v);         \
                                                      \
   auto macro = mme_builder_finish_vec(&b);           \
                                                      \
   uint32_t end = (uint32_t)(start), count = 0;       \
   while (c_##op(end, (bound))) {                     \
      end += (uint32_t)(step);                        \
      count++;                                        \
   }                                                  \
                                                      \
   std::vector<uint32_t> params;                      \
   test_macro(&b, macro, params);                     \
   ASSERT_EQ(data[0], 5);                             \
   ASSERT_EQ(data[1], 5 * count);                     \
   ASSERT_EQ(data[2], end);                           \
   ASSERT_EQ(data[3], 5 * count);                     \
   ASSERT_EQ(data[4], 5);                             \
}

WHILE_TEST(ilt, 0, 1, 7)
WHILE_TEST(ult, 0, 1, 7)
WHILE_TEST(ile, -10, 2, 0)
WHILE_TEST(ule, 0, 1, 7)
WHILE_TEST(ieq, 0, 5, 0)
WHILE_TEST(ige, 5, -1, -5)
WHILE_TEST(uge, 15, -2, 2)
WHILE_TEST(igt, 7, -3, -10)
WHILE_TEST(ugt, 1604, -30, 1000)
WHILE_TEST(ine, 0, 1, 7)

#undef WHILE_TEST

#if 0
TEST_F(mme_tu104_sim_test, do_ble)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_alu(&b, R5, ADD, LOAD0, ZERO);
   mme_alu(&b, R6, ADD, ZERO, ZERO);
   mme_alu(&b, R7, ADD, ZERO, ZERO);

   mme_alu_imm(&b, R7, ADD, R7, IMM, 1);
   mme_alu_imm(&b, R7, ADD, R7, IMM, 1);
   mme_alu_imm(&b, R7, ADD, R7, IMM, 1);
   mme_alu_imm(&b, R7, ADD, R7, IMM, 1);
   mme_alu_imm(&b, R6, ADD, R6, IMM, 1);
   mme_branch(&b, BLE, R6, R5, -3, 2);
   mme_alu_imm(&b, R7, ADD, R7, IMM, 1);
   mme_alu_imm(&b, R7, ADD, R7, IMM, 1);

   mme_store_imm_addr(&b, data_addr + 0,  MME_TU104_REG_R7);

   mme_end(&b);

   uint32_t counts[] = {0, 1, 5, 9};

   for (uint32_t i = 0; i < ARRAY_SIZE(counts); i++) {
      reset_push();

      std::vector<uint32_t> params;
      params.push_back(counts[i]);

      test_macro(&b, params);
   }
}
#endif

TEST_F(mme_tu104_sim_test, dread_dwrite)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);

   mme_dwrite(&b, mme_imm(5), x);
   mme_dwrite(&b, mme_imm(8), y);

   mme_value y2 = mme_dread(&b, mme_imm(8));
   mme_value x2 = mme_dread(&b, mme_imm(5));

   mme_store_imm_addr(&b, data_addr + 0, y2);
   mme_store_imm_addr(&b, data_addr + 4, x2);

   auto macro = mme_builder_finish_vec(&b);

   std::vector<uint32_t> params;
   params.push_back(-10);
   params.push_back(5);

   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, dwrite_dma)
{
   const uint32_t canary5 = 0xc0ffee01;
   const uint32_t canary8 = canary5 & 0x00ffff00;

   mme_builder b;
   mme_builder_init(&b);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);

   mme_dwrite(&b, mme_imm(5), x);
   mme_dwrite(&b, mme_imm(8), y);

   auto macro = mme_builder_finish_vec(&b);

   push_macro(0, macro);

   P_1INC(p, NVC597, CALL_MME_MACRO(0));
   P_INLINE_DATA(p, canary5);
   P_INLINE_DATA(p, canary8);

   P_MTHD(p, NVC597, SET_MME_MEM_ADDRESS_A);
   P_NVC597_SET_MME_MEM_ADDRESS_A(p, high32(data_addr));
   P_NVC597_SET_MME_MEM_ADDRESS_B(p, low32(data_addr));
   /* Start 3 dwords into MME RAM */
   P_NVC597_SET_MME_DATA_RAM_ADDRESS(p, 3);
   P_IMMD(p, NVC597, MME_DMA_WRITE, 20);

   submit_push();

   for (uint32_t i = 0; i < 20; i++) {
      if (i == 5 - 3) {
         ASSERT_EQ(data[i], canary5);
      } else if (i == 8 - 3) {
         ASSERT_EQ(data[i], canary8);
      } else {
         ASSERT_EQ(data[i], 0);
      }
   }
}

TEST_F(mme_tu104_sim_test, dram_limit)
{
   static const uint32_t chunk_size = 32;

   mme_builder b;
   mme_builder_init(&b);

   mme_value start = mme_load(&b);
   mme_value count = mme_load(&b);

   mme_value i = mme_mov(&b, start);
   mme_loop(&b, count) {
      mme_dwrite(&b, i, i);
      mme_add_to(&b, i, i, mme_imm(1));
   }

   mme_value j = mme_mov(&b, start);
   struct mme_value64 addr = mme_mov64(&b, mme_imm64(data_addr));

   mme_loop(&b, count) {
      mme_value x = mme_dread(&b, j);
      mme_store(&b, addr, x);
      mme_add_to(&b, j, j, mme_imm(1));
      mme_add64_to(&b, addr, addr, mme_imm64(4));
   }

   auto macro = mme_builder_finish_vec(&b);

   for (uint32_t i = 0; i < MME_TU104_DRAM_COUNT; i += chunk_size) {
      reset_push();

      push_macro(0, macro);

      P_1INC(p, NVC597, CALL_MME_MACRO(0));
      P_INLINE_DATA(p, i);
      P_INLINE_DATA(p, chunk_size);

      submit_push();

      for (uint32_t j = 0; j < chunk_size; j++)
         ASSERT_EQ(data[j], i + j);
   }
}

TEST_F(mme_tu104_sim_test, dma_read_fifoed)
{
   mme_builder b;
   mme_builder_init(&b);

   mme_mthd(&b, NVC597_SET_MME_DATA_RAM_ADDRESS);
   mme_emit(&b, mme_zero());

   mme_mthd(&b, NVC597_SET_MME_MEM_ADDRESS_A);
   mme_emit(&b, mme_imm(high32(data_addr)));
   mme_emit(&b, mme_imm(low32(data_addr)));

   mme_mthd(&b, NVC597_MME_DMA_READ_FIFOED);
   mme_emit(&b, mme_imm(2));

   mme_tu104_alu_no_dst(&b, MME_TU104_ALU_OP_EXTENDED,
                        mme_imm(0x1000), mme_imm(1), 0);

   mme_value x = mme_load(&b);
   mme_value y = mme_load(&b);

   mme_store_imm_addr(&b, data_addr + 256 + 0, x);
   mme_store_imm_addr(&b, data_addr + 256 + 4, y);

   auto macro = mme_builder_finish_vec(&b);

   P_IMMD(p, NVC597, SET_MME_DATA_FIFO_CONFIG, FIFO_SIZE_SIZE_4KB);

   for (uint32_t i = 0; i < 64; i++)
      data[i] = 1000 + i;

   std::vector<uint32_t> params;
   params.push_back(7);

   test_macro(&b, macro, params);
}

TEST_F(mme_tu104_sim_test, scratch_limit)
{
   static const uint32_t chunk_size = 32;

   mme_builder b;
   mme_builder_init(&b);

   mme_value start = mme_load(&b);
   mme_value count = mme_load(&b);

   mme_value i = mme_mov(&b, start);
   mme_loop(&b, count) {
      mme_mthd_arr(&b, NVC597_SET_MME_SHADOW_SCRATCH(0), i);
      mme_emit(&b, i);
      mme_add_to(&b, i, i, mme_imm(1));
   }

   mme_value j = mme_mov(&b, start);
   struct mme_value64 addr = mme_mov64(&b, mme_imm64(data_addr));

   mme_loop(&b, count) {
      mme_value x = mme_state_arr(&b, NVC597_SET_MME_SHADOW_SCRATCH(0), j);
      mme_store(&b, addr, x);
      mme_add_to(&b, j, j, mme_imm(1));
      mme_add64_to(&b, addr, addr, mme_imm64(4));
   }

   auto macro = mme_builder_finish_vec(&b);

   for (uint32_t i = 0; i < MME_TU104_SCRATCH_COUNT; i += chunk_size) {
      reset_push();

      push_macro(0, macro);

      P_1INC(p, NVC597, CALL_MME_MACRO(0));
      P_INLINE_DATA(p, i);
      P_INLINE_DATA(p, chunk_size);

      submit_push();

      for (uint32_t j = 0; j < chunk_size; j++)
         ASSERT_EQ(data[j], i + j);
   }
}
