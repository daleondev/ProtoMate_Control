"""Frozen TraceX layout, rollover, corruption and cross-core correlation checks."""
from pathlib import Path
import struct
import sys

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from profiling.decode import native, timeline, control, summary


def fixture(*, wrap=False, timestamps=(0xfffffff0,0x10), started=0xffffffe0, frozen=0x20):
    base=0xc0400000
    data=bytearray(48+48+4*32)
    struct.pack_into('<4I2H7I',data,0,0x54585442,0xffffffff,base,base+48,0,32,
                     base+96,base+96,base+224,base+160,0,0,0)
    for index,timestamp in enumerate(timestamps):
        struct.pack_into('<8I',data,96+index*32,0x1234,9,4122,timestamp,2,77,0,0)
    status=dict(wraps=int(wrap),started_us=started,frozen_us=frozen,capture=1)
    return data,status


def test_rollover_is_reconstructed_from_freeze_anchor():
    data,status=fixture()
    parsed=native(data,status,1)
    assert [event['capture_us'] for event in parsed['records']] == [16,48]
    assert not parsed['overwritten']


def test_overwrite_uses_ring_order_and_null_slots():
    data,status=fixture(wrap=True, timestamps=(30,40),started=0,frozen=50)
    struct.pack_into('<8I',data,160,0x1234,9,4122,10,2,77,0,0)
    struct.pack_into('<8I',data,192,0x1234,9,4122,20,2,77,0,0)
    assert [e['timestamp'] for e in native(data,status)['records']] == [10,20,30,40]


@pytest.mark.parametrize('offset,value',[(0,0),(4,0),(12,0),(28,0xffffffff),(32,0xc0400061)])
def test_corrupt_header_rejected(offset,value):
    data,status=fixture()
    struct.pack_into('<I',data,offset,value)
    with pytest.raises(ValueError): native(data,status)


def test_one_way_correlation_uses_actual_peer_timestamps():
    def capture(core,received,time):
        return dict(status=dict(started_us=100,capture=1),trace=dict(objects={},records=[
            dict(thread=123,priority=9,event=4122,timestamp=time,capture_us=time,
                 values=[2,77,0,received])]))
    _,report=timeline({7:capture(7,0,10),4:capture(4,1,42)})
    assert report['matched']==1 and report['maximum']==32
    _,report=timeline({4:capture(4,1,42)})
    assert report['matched']==0 and report['unmatched']==1


def test_control_and_summary_bounds():
    with pytest.raises(ValueError):control(bytes(163))
    with pytest.raises(ValueError):control(bytes(164))
    with pytest.raises(ValueError):summary(bytes(8231),{})


def control_fixture(core=4, capture=1):
    words = [0]*41
    words[:4] = [0x50524631, 1, 112, 8232]
    words[10:13] = [131072, 256, 4]
    words[13:21] = [3, 0, 0, capture, core, 240000000, 0, 50]
    return struct.pack('<41I', *words)


def test_valid_control_preserves_failure_details_and_rejects_torn_write():
    data = bytearray(control_fixture())
    struct.pack_into('<8I', data, 132, 2, 42, 3, 4096, 0x90101234, 0x24040000, 1, 1)
    parsed = control(data)
    assert parsed['status']['failure']['caller'] == 0x90101234
    assert parsed['status']['failure']['size'] == 4096
    struct.pack_into('<I', data, 132, 3)
    with pytest.raises(ValueError, match='incomplete'):
        control(data)


def test_host_word_layout_cannot_masquerade_as_hardware_capture():
    data = bytearray(control_fixture())
    struct.pack_into('<I', data, 48, 8)
    with pytest.raises(ValueError, match='32-bit'):
        control(data)


def test_summary_keeps_cycles_and_duration_across_timer_wrap():
    data = bytearray(8232)
    struct.pack_into('<4IQ16I', data, 0, 2, 240, 480, 0, 720, *([2]+[0]*15))
    struct.pack_into('<6I', data, 64*88, 1, 4, 4, 3, 0, 2)
    struct.pack_into('<3Q4I', data, 8192, 0, 0, 0, 0, 0, 1, 1)
    result = summary(data, dict(frequency_hz=240000000, started_us=0xfffffff0, frozen_us=0x20))
    assert result['capture_duration_us'] == 48
    assert result['sites'][0]['mean_us'] == 1.5
    assert result['sites'][0]['max_us'] == 2
    assert result['queues'][0]['blocked'] == 3
    assert result['queues'][0]['replaced'] == 2


def test_export_never_merges_mismatched_sessions(tmp_path):
    from profiling.decode import export
    for core in (4, 7):
        (tmp_path/f'm{core}.control').write_bytes(control_fixture(core, core))
        (tmp_path/f'm{core}.summary').write_bytes(bytes(8232))
        data, _ = fixture(timestamps=(10, 20), started=0, frozen=50)
        (tmp_path/f'm{core}.trx').write_bytes(data)
    with pytest.raises(ValueError, match='different sessions'):
        export(tmp_path)
    assert not (tmp_path/'timeline.json').exists()


def test_native_event_catalog_does_not_confuse_configuration_constants(tmp_path):
    from profiling.decode import kernel_catalog
    header = tmp_path/'tx_trace.h'
    header.write_text('''#define TX_TRACE_THREAD_RESUME 1 /* I1 = thread ptr */
#define TX_TRACE_ISR_ENTER 3 /* I1 = stack_ptr */
#define TX_TRACE_OBJECT_REGISTRY_NAME 32
#define TX_TRACE_USER_EVENT_START 4096 /* lower bound */
''')
    assert kernel_catalog(header) == {1: 'ThreadX thread resume', 3: 'ThreadX isr enter'}


def test_calibration_does_not_claim_a_microsecond_histogram():
    data = bytearray(8232)
    struct.pack_into('<4IQ16I', data, 63*88, 1, 2400, 2400, 0, 2400, *([1]+[0]*15))
    result = summary(data, dict(frequency_hz=240000000, started_us=0, frozen_us=20))
    assert result['sites'][0]['mean_us'] == 10
    assert result['sites'][0]['histogram'] is None


def test_debugger_storage_must_be_uncached_and_fit_its_region():
    from profiling.decode import validate_debug_address
    validate_debug_address(4, 0x3003f000, 164)
    validate_debug_address(7, 0x20000800, 164)
    for core, address, size in [(7, 0x240003a8, 164), (7, 0x2001fff0, 164),
                                (4, 0x24040000, 24), (7, 0x20000800, 0)]:
        with pytest.raises(ValueError, match='uncached'):
            validate_debug_address(core, address, size)
