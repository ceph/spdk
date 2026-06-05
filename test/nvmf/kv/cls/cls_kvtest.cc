// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab
//
// KVX-3 THROWAWAY test object class for exercising the KV Exec -> rados_aio_exec
// path end to end against a vstart cluster. NOT a production class (ADR-0005
// keeps real cls authoring to a later effort).
//
// Methods:
//   echo   - return the caller's input blob unchanged (no object access).
//   upcase - read the object's stored value and return it uppercased; if the
//            object is empty/absent, uppercase the caller's input instead.
//
// Build against the Ceph tree at /mnt/ceph; load libcls_kvtest.so into the
// vstart OSD (see test/nvmf/kv/cls/build_and_load.sh).

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <string>

#include "objclass/objclass.h"

CLS_VER(1, 0)
CLS_NAME(kvtest)

// echo: return the input bytes straight back to the caller. Does not touch the
// object, so it is registered RD-only (actually neither RD nor WR is needed,
// but RD is harmless and matches the "may read" contract).
static int kvtest_echo(cls_method_context_t hctx, ceph::bufferlist *in,
                       ceph::bufferlist *out)
{
  CLS_LOG(10, "kvtest echo: %u input bytes", (unsigned)in->length());
  *out = *in;
  return 0;
}

// upcase: read the stored object value and return it uppercased. If the object
// has no value, fall back to uppercasing the input blob. Reads the object, so
// it is registered RD.
static int kvtest_upcase(cls_method_context_t hctx, ceph::bufferlist *in,
                         ceph::bufferlist *out)
{
  ceph::bufferlist stored;
  int r = cls_cxx_read(hctx, 0, 0, &stored);
  if (r < 0 && r != -ENOENT) {
    CLS_ERR("kvtest upcase: read failed: %d", r);
    return r;
  }

  std::string s;
  if (stored.length() > 0) {
    s.assign(stored.c_str(), stored.length());
  } else {
    s.assign(in->c_str(), in->length());
  }
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::toupper(c); });

  out->clear();
  out->append(s.data(), s.size());
  CLS_LOG(10, "kvtest upcase: returning %u bytes", (unsigned)out->length());
  return 0;
}

CLS_INIT(kvtest)
{
  CLS_LOG(0, "loading cls_kvtest");

  cls_handle_t h_class;
  cls_method_handle_t h_echo;
  cls_method_handle_t h_upcase;

  cls_register("kvtest", &h_class);
  detail::cls_register_cxx_method_impl(h_class, "echo", CLS_METHOD_RD,
                                       kvtest_echo, &h_echo);
  detail::cls_register_cxx_method_impl(h_class, "upcase", CLS_METHOD_RD,
                                       kvtest_upcase, &h_upcase);
}
