# This file was automatically generated by SWIG (http://www.swig.org).
# Version 4.0.2
#
# Do not make changes to this file unless you know what you are doing--modify
# the SWIG interface file instead.

from sys import version_info as _swig_python_version_info
if _swig_python_version_info < (2, 7, 0):
    raise RuntimeError("Python 2.7 or later required")

# Import the low-level C/C++ module
if __package__ or "." in __name__:
    from . import _chmlib
else:
    import _chmlib

try:
    import builtins as __builtin__
except ImportError:
    import __builtin__

def _swig_repr(self):
    try:
        strthis = "proxy of " + self.this.__repr__()
    except __builtin__.Exception:
        strthis = ""
    return "<%s.%s; %s >" % (self.__class__.__module__, self.__class__.__name__, strthis,)


def _swig_setattr_nondynamic_instance_variable(set):
    def set_instance_attr(self, name, value):
        if name == "thisown":
            self.this.own(value)
        elif name == "this":
            set(self, name, value)
        elif hasattr(self, name) and isinstance(getattr(type(self), name), property):
            set(self, name, value)
        else:
            raise AttributeError("You cannot add instance attributes to %s" % self)
    return set_instance_attr


def _swig_setattr_nondynamic_class_variable(set):
    def set_class_attr(cls, name, value):
        if hasattr(cls, name) and not isinstance(getattr(cls, name), property):
            set(cls, name, value)
        else:
            raise AttributeError("You cannot add class attributes to %s" % cls)
    return set_class_attr


def _swig_add_metaclass(metaclass):
    """Class decorator for adding a metaclass to a SWIG wrapped class - a slimmed down version of six.add_metaclass"""
    def wrapper(cls):
        return metaclass(cls.__name__, cls.__bases__, cls.__dict__.copy())
    return wrapper


class _SwigNonDynamicMeta(type):
    """Meta class to enforce nondynamic attributes (no new attributes) for a class"""
    __setattr__ = _swig_setattr_nondynamic_class_variable(type.__setattr__)


CHM_UNCOMPRESSED = _chmlib.CHM_UNCOMPRESSED
CHM_COMPRESSED = _chmlib.CHM_COMPRESSED
CHM_MAX_PATHLEN = _chmlib.CHM_MAX_PATHLEN
class chmUnitInfo(object):
    thisown = property(lambda x: x.this.own(), lambda x, v: x.this.own(v), doc="The membership flag")
    __repr__ = _swig_repr
    start = property(_chmlib.chmUnitInfo_start_get, _chmlib.chmUnitInfo_start_set)
    length = property(_chmlib.chmUnitInfo_length_get, _chmlib.chmUnitInfo_length_set)
    space = property(_chmlib.chmUnitInfo_space_get, _chmlib.chmUnitInfo_space_set)
    path = property(_chmlib.chmUnitInfo_path_get, _chmlib.chmUnitInfo_path_set)

    def __init__(self):
        _chmlib.chmUnitInfo_swiginit(self, _chmlib.new_chmUnitInfo())
    __swig_destroy__ = _chmlib.delete_chmUnitInfo

# Register chmUnitInfo in _chmlib:
_chmlib.chmUnitInfo_swigregister(chmUnitInfo)


def chm_open(filename):
    return _chmlib.chm_open(filename)

def chm_close(h):
    return _chmlib.chm_close(h)
CHM_PARAM_MAX_BLOCKS_CACHED = _chmlib.CHM_PARAM_MAX_BLOCKS_CACHED

def chm_set_param(h, paramType, paramVal):
    return _chmlib.chm_set_param(h, paramType, paramVal)
CHM_RESOLVE_SUCCESS = _chmlib.CHM_RESOLVE_SUCCESS
CHM_RESOLVE_FAILURE = _chmlib.CHM_RESOLVE_FAILURE

def chm_resolve_object(h, objPath):
    return _chmlib.chm_resolve_object(h, objPath)

def chm_retrieve_object(h, ui, addr, len):
    return _chmlib.chm_retrieve_object(h, ui, addr, len)
CHM_ENUMERATE_NORMAL = _chmlib.CHM_ENUMERATE_NORMAL
CHM_ENUMERATE_META = _chmlib.CHM_ENUMERATE_META
CHM_ENUMERATE_SPECIAL = _chmlib.CHM_ENUMERATE_SPECIAL
CHM_ENUMERATE_FILES = _chmlib.CHM_ENUMERATE_FILES
CHM_ENUMERATE_DIRS = _chmlib.CHM_ENUMERATE_DIRS
CHM_ENUMERATE_ALL = _chmlib.CHM_ENUMERATE_ALL
CHM_ENUMERATOR_FAILURE = _chmlib.CHM_ENUMERATOR_FAILURE
CHM_ENUMERATOR_CONTINUE = _chmlib.CHM_ENUMERATOR_CONTINUE
CHM_ENUMERATOR_SUCCESS = _chmlib.CHM_ENUMERATOR_SUCCESS

def chm_enumerate(h, what, e, context):
    return _chmlib.chm_enumerate(h, what, e, context)

def chm_enumerate_dir(h, prefix, what, e, context):
    return _chmlib.chm_enumerate_dir(h, prefix, what, e, context)


