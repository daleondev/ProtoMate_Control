#!/usr/bin/env python3
"""Compile the real MPU setup against a recording HAL, for both cores/helper."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
FIELDS='Enable Number BaseAddress Size TypeExtField AccessPermission DisableExec IsShareable IsCacheable IsBufferable SubRegionDisable'.split()
class Region(ctypes.Structure):
    _fields_=[(name,ctypes.c_uint32) for name in FIELDS]

class MpuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary=tempfile.TemporaryDirectory(prefix='giga-mpu-')
        directory=Path(cls.temporary.name)
        constants=dict(MPU_REGION_ENABLE=1,MPU_TEX_LEVEL1=1,MPU_INSTRUCTION_ACCESS_ENABLE=0,
                       MPU_INSTRUCTION_ACCESS_DISABLE=1,MPU_ACCESS_NOT_SHAREABLE=0,
                       MPU_ACCESS_SHAREABLE=1,MPU_ACCESS_CACHEABLE=1,MPU_ACCESS_NOT_CACHEABLE=0,
                       MPU_ACCESS_BUFFERABLE=1,MPU_ACCESS_NOT_BUFFERABLE=0,
                       MPU_REGION_FULL_ACCESS=3,MPU_REGION_PRIV_RO_URO=6,MPU_PRIVILEGED_DEFAULT=4,MPU_HFNMI_PRIVDEF=6)
        for name,size in [('64KB',65536),('128KB',131072),('256KB',262144),('512KB',524288),('2MB',2097152),('8MB',8388608),('16MB',16777216)]:
            constants['MPU_REGION_SIZE_'+name]=size
        header='#include <stdint.h>\ntypedef struct {'+''.join('uint32_t '+n+';' for n in FIELDS)+'} MPU_Region_InitTypeDef;\n'
        header+='\n'.join(f'#define {name} {value}u' for name,value in constants.items())
        header+='\nvoid HAL_MPU_ConfigRegion(MPU_Region_InitTypeDef*);\nvoid HAL_MPU_Disable(void);\nvoid HAL_MPU_Enable(uint32_t);\n#define __DSB() ((void)0)\n#define __ISB() ((void)0)\n'
        (directory/'stm32h7xx_hal.h').write_text(header)
        (directory/'record.c').write_text('#include "stm32h7xx_hal.h"\nMPU_Region_InitTypeDef regions[16];\nuint32_t control;\n'
            'void HAL_MPU_ConfigRegion(MPU_Region_InitTypeDef* r){regions[r->Number]=*r;}\n'
            'void HAL_MPU_Disable(void){}\nvoid HAL_MPU_Enable(uint32_t v){control=v;}\n')
        cls.libraries={}
        for variant,definitions in [('m4',['CORE_CM4']),('m7',['CORE_CM7']),('helper',['CORE_CM7','GIGA_MEMORY_HELPER'])]:
            output=directory/(variant+'.so')
            subprocess.run(['cc','-Wall','-Wextra','-Werror','-shared','-fPIC',*['-D'+d for d in definitions],
                            '-I'+str(directory),'-I'+str(ROOT/'platform'),str(ROOT/'platform/boot/giga/mpu.c'),str(directory/'record.c'),'-o',str(output)],check=True)
            cls.libraries[variant]=ctypes.CDLL(str(output))
    @classmethod
    def tearDownClass(cls):cls.temporary.cleanup()
    def regions(self,variant,debug=0):
        library=self.libraries[variant];library.giga_configure_mpu(debug)
        return (Region*16).in_dll(library,'regions')
    def test_maintenance_code_is_executable_but_application_data_is_not(self):
        self.assertEqual(self.regions('helper')[5].DisableExec,0)
        self.assertEqual(self.regions('m7')[5].DisableExec,1)
        self.assertEqual(self.regions('m4')[5].DisableExec,1)
    def test_framebuffer_and_shared_queues_are_normal_noncacheable_xn(self):
        for variant in self.libraries:
            for index in [0,2]:
                r=self.regions(variant)[index]
                self.assertEqual((r.TypeExtField,r.IsCacheable,r.IsShareable,r.DisableExec),(1,0,1,1))
    def test_debug_code_only_executable_when_requested(self):
        self.assertEqual(self.regions('m4')[3].DisableExec,1)
        self.assertEqual(self.regions('m4',1)[3].DisableExec,0)
    def test_m4_data_and_stack_are_xn_while_sram1_is_excluded(self):
        r=self.regions('m4')[7]
        self.assertEqual((r.BaseAddress,r.Size,r.SubRegionDisable,r.DisableExec),(0x30000000,512*1024,3,1))
    def test_flash_is_read_only_and_cacheable(self):
        r=self.regions('m7')[1]
        self.assertEqual((r.BaseAddress,r.Size,r.AccessPermission,r.IsCacheable,r.DisableExec),(0x90000000,16*1024*1024,6,1,0))
    def test_internal_storage_is_noncacheable_and_not_executable(self):
        r=self.regions('m7')[8]
        self.assertEqual((r.BaseAddress,r.Size,r.AccessPermission,r.IsCacheable,r.DisableExec),(0x08180000,512*1024,3,0,1))
        self.assertEqual(self.regions('m4')[8].Enable,0)
    def test_ecc_probes_keep_mpu_attributes_at_fault_priority(self):
        for variant in ['m7','helper']:
            self.regions(variant)
            self.assertEqual(ctypes.c_uint32.in_dll(self.libraries[variant],'control').value,6)
if __name__=='__main__':unittest.main()
