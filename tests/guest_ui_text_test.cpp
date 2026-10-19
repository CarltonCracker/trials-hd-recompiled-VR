// trials_hd - guest UI setter ABI/UTF-16 regression; no game files required.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "guest_ui_text.h"

namespace {
void Check(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}

int calls = 0;
std::u16string copied_text;
int32_t result = 0;

void CopyingSetter(PPCContext& ctx, uint8_t* base) {
  ++calls;
  Check(ctx.r1.u32 == 0x700, "Reserve an aligned guest call frame");
  Check(rex::memory::load_and_swap<uint32_t>(base + ctx.r1.u32) == 0x800,
        "Preserve the guest stack backchain");
  Check(ctx.r3.u32 == 0x1234, "Pass the selected control, not another menu item");
  Check(ctx.r4.u32 == 0x780, "Place text above callee save/linkage slots");
  Check(base[ctx.r4.u32] == 0 && base[ctx.r4.u32 + 1] == 'P', "Encode UTF-16 big-endian");
  copied_text.clear();
  for (uint32_t address = ctx.r4.u32;; address += 2) {
    const auto c = rex::memory::load_and_swap<uint16_t>(base + address);
    if (!c) break;
    copied_text.push_back(char16_t(c));
  }
  ctx = PPCContext{};  // Guest callees may clobber registers.
  ctx.r3.s64 = result;
}
}

int main() {
  std::array<uint8_t, 4096> memory{};
  PPCContext caller{};
  caller.r1.u64 = 0x800;
  caller.r3.u64 = 0xDEADBEEF;
  caller.r4.u64 = 0xCAFEBABE;
  caller.lr = 0x82123456;
  std::array<uint8_t, sizeof(PPCContext)> saved;
  std::memcpy(saved.data(), &caller, sizeof(caller));

  Check(trials_hd::SetGuestControlText(caller, memory.data(), 0x1234, u"PC SETTINGS", CopyingSetter),
        "Set the main-menu caption");
  Check(copied_text == u"PC SETTINGS", "Setter receives the full terminated caption");
  Check(std::memcmp(saved.data(), &caller, sizeof(caller)) == 0, "Preserve the caller context");
  Check(trials_hd::SetGuestControlText(caller, memory.data(), 0x1234, u"PC \u03A9", CopyingSetter),
        "Encode non-ASCII UTF-16 too");
  Check(copied_text == u"PC \u03A9", "Keep both bytes of wide characters");

  result = int32_t(0x80070057u);
  Check(!trials_hd::SetGuestControlText(caller, memory.data(), 0x1234, u"PC SETTINGS", CopyingSetter),
        "Report guest setter failure");
  Check(std::memcmp(saved.data(), &caller, sizeof(caller)) == 0, "Preserve context on failure");
  const int before = calls;
  Check(!trials_hd::SetGuestControlText(caller, memory.data(), 0, u"PC SETTINGS", CopyingSetter),
        "Ignore a missing control");
  const char16_t unterminated[] = {u'P', u'C'};
  Check(!trials_hd::SetGuestControlText(caller, memory.data(), 0x1234, unterminated, CopyingSetter),
        "Reject unterminated text");
  caller.r1.u64 = 0x808;
  Check(!trials_hd::SetGuestControlText(caller, memory.data(), 0x1234, u"PC SETTINGS", CopyingSetter),
        "Reject a misaligned guest stack");
  caller.r1.u64 = 0;
  Check(!trials_hd::SetGuestControlText(caller, memory.data(), 0x1234, u"PC SETTINGS", CopyingSetter),
        "Reject stack underflow");
  Check(calls == before, "Do not invoke the setter for invalid arguments");
}
