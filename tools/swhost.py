"""Minimal Schwung audio-FX host, for the build gates.

dlopen()s the real .so the way the chain host does and exposes the v2 API
through ctypes. Used by gen_module_json.py and check_contract.py.
"""
import ctypes as C

CREATE = C.CFUNCTYPE(C.c_void_p, C.c_char_p, C.c_char_p)
DESTROY = C.CFUNCTYPE(None, C.c_void_p)
PROCESS = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(C.c_int16), C.c_int)
SETP = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p, C.c_char_p)
GETP = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p, C.c_char_p, C.c_int)
MIDI = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p, C.c_int, C.c_int)
LOG = C.CFUNCTYPE(None, C.c_char_p)


class ApiV2(C.Structure):
    _fields_ = [("api_version", C.c_uint32),
                ("create_instance", CREATE),
                ("destroy_instance", DESTROY),
                ("process_block", PROCESS),
                ("set_param", SETP),
                ("get_param", GETP),
                ("on_midi", MIDI)]


class HostApiPrefix(C.Structure):
    """Leading fields of host_api_v1_t: enough for sample_rate and log."""
    _fields_ = [("api_version", C.c_uint32),
                ("sample_rate", C.c_int),
                ("frames_per_block", C.c_int),
                ("mapped_memory", C.c_void_p),
                ("audio_out_offset", C.c_int),
                ("audio_in_offset", C.c_int),
                ("log", LOG)]


class Fx:
    def __init__(self, so_path, sample_rate=44100):
        self.lib = C.CDLL(so_path)
        init = self.lib.move_audio_fx_init_v2
        init.restype = C.POINTER(ApiV2)
        init.argtypes = [C.c_void_p]
        self.logs = []
        self._log = LOG(lambda m: self.logs.append(m.decode()))
        # Pad well past the real struct so the plugin never reads off the end.
        buf = C.create_string_buffer(512)
        host = C.cast(buf, C.POINTER(HostApiPrefix)).contents
        host.api_version = 1
        host.sample_rate = sample_rate
        host.frames_per_block = 128
        host.log = self._log
        self._host_buf = buf
        self.api = init(C.addressof(buf)).contents
        self.inst = self.api.create_instance(b".", None)
        assert self.inst, "create_instance returned NULL"

    def new_instance(self):
        i = Fx.__new__(Fx)
        i.lib, i.api, i.logs, i._log, i._host_buf = self.lib, self.api, self.logs, self._log, self._host_buf
        i.inst = self.api.create_instance(b".", None)
        return i

    def get(self, key, cap=16384):
        buf = C.create_string_buffer(cap)
        n = self.api.get_param(self.inst, key.encode(), buf, cap)
        return None if n < 0 else buf.value.decode()

    def set(self, key, val):
        self.api.set_param(self.inst, key.encode(), str(val).encode())

    def process(self, samples):
        """samples: array('h') of interleaved stereo, processed in place."""
        n = len(samples) // 2
        ptr = (C.c_int16 * len(samples)).from_buffer(samples)
        done = 0
        while done < n:
            blk = min(128, n - done)
            self.api.process_block(self.inst, C.cast(C.byref(ptr, done * 4), C.POINTER(C.c_int16)), blk)
            done += blk

    def destroy(self):
        self.api.destroy_instance(self.inst)
