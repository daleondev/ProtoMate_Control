#pragma once
/* Force-included by the M4 build only for objects placed wholly in internal
   instruction RAM, including portable sources without CORE_CM4 defined. Arm EHABI
   encodes personality addresses as signed PREL31; use a nearby tail-call veneer
   rather than an out-of-range reference to the QSPI C++ runtime. */
#if defined(__cplusplus)
asm(".set __gxx_personality_v0, __giga_fast_personality");
#endif
