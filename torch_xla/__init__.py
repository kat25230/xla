import os
import re
import tempfile
import subprocess

XRT_RUN_SERVER_PROCESS = 'torch_xla.core._xrt_run_server'
XRT_SERVER_REGEX = '^python3 -m {} [0-9]+$'.format(XRT_RUN_SERVER_PROCESS)


def server_is_alive():
  # pgrep returns 0 when at least one running process matches the requested name.
  # Otherwise, the exit code is 1. If pgrep is not availiable in the system, it
  # will return an exit code 127.
  return subprocess.getstatusoutput(
      'pgrep -f "{}"'.format(XRT_SERVER_REGEX))[0] == 0


def _setup_grpc():
  # Setup GRPC options to correctly talk to TPU backends.
  options = [
      'grpc.keepalive_time_ms=60000',  # 1 min
      'grpc.keepalive_timeout_ms=14400000',  # 4 hrs
      'grpc.http2.max_pings_without_data=0',  # unlimited
      'grpc.http2.min_ping_interval_without_data_ms=300000',  # 5 min
  ]
  os.environ['TF_GRPC_DEFAULT_OPTIONS'] = ','.join(options)


def _set_missing_flags(flags, sets):
  for name, defval in sets:
    insert = True
    for fval in flags:
      m = re.match(r'(--)?([^=]+)', fval)
      if m and m.group(2) == name:
        insert = False
        break
    if insert:
      flags.append('--{}={}'.format(name, defval))
  return flags


def _setup_xla_flags():
  flags = os.environ.get('XLA_FLAGS', '').split(' ')
  flags = _set_missing_flags(flags, (('xla_cpu_enable_fast_math', 'false'),))
  os.environ['XLA_FLAGS'] = ' '.join(flags)


def _set_missing_env(name, value):
  if name not in os.environ:
    os.environ[name] = value


def _setup_default_env():
  _set_missing_env('TF_CPP_MIN_LOG_LEVEL', '1')
  _set_missing_env('TPU_HOST_BOUNDS', '1,1,1')
  _set_missing_env('GRPC_VERBOSITY', 'ERROR')
  _set_missing_env('ALLOW_MULTIPLE_LIBTPU_LOAD', '1')
  if server_is_alive():
    _set_missing_env('XRT_START_LOCAL_SERVER', '0')


_fd, _tmp_fname = -1, ''


def _setup_debug_env():
  fd, tmp_fname = tempfile.mkstemp('.ptxla', text=True)
  _set_missing_env('XLA_FNTRACKER_FILE', tmp_fname)
  return fd, tmp_fname


def _summarize_fn_tracker():
  if not _tmp_fname:
    return
  from .debug.frame_parser_util import process_frames
  process_frames(_tmp_fname)
  os.close(_fd)
  os.remove(_tmp_fname)


def _tpu_vm_init():
  try:
    import libtpu
    libtpu.configure_library_path()
  except ImportError:
    return


# These needs to be called before the _XLAC module is loaded.
_setup_default_env()
_setup_grpc()
_setup_xla_flags()
if int(os.environ.get('PT_XLA_DEBUG', '0')):
  _fd, _tmp_fname = _setup_debug_env()

# Setup libtpu library for TPU VM.
_tpu_vm_init()

import atexit
import torch
from ._patched_functions import _apply_patches
from .version import __version__
import _XLAC


def _prepare_to_exit():
  _XLAC._prepare_to_exit()
  if int(os.environ.get('PT_XLA_DEBUG', '0')):
    _summarize_fn_tracker()


atexit.register(_prepare_to_exit)
_apply_patches()
