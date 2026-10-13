#pragma once
/* Applied only to objects placed wholly in internal instruction RAM. Arm EHABI
   encodes personality addresses as signed PREL31; use a nearby tail-call veneer
   rather than an out-of-range reference to the QSPI C++ runtime. */
#if defined(__cplusplus) && defined(CORE_CM4)
asm(".set __gxx_personality_v0, __giga_fast_personality");
#endif
