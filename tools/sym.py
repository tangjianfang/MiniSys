# Resolve an RVA in a module to a symbol via dbghelp (ctypes).
import ctypes, sys
from ctypes import wintypes

exe = r"C:\tjf\github\MiniSys\build\p7\MiniSys.pdb"
rva = int(sys.argv[1], 16)

dbghelp = ctypes.WinDLL("dbghelp.dll")
kernel32 = ctypes.WinDLL("kernel32.dll")
h = kernel32.GetCurrentProcess()

class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [
        ("sizeOfStruct", wintypes.ULONG), ("typeIndex", wintypes.ULONG),
        ("Reserved1", ctypes.c_ulonglong), ("Reserved2", ctypes.c_ulonglong),
        ("Reserved3", ctypes.c_ulonglong), ("Reserved4", ctypes.c_ulonglong),
        ("Index", wintypes.ULONG), ("Size", wintypes.ULONG),
        ("ModBase", ctypes.c_ulonglong), ("Flags", wintypes.ULONG),
        ("Value", ctypes.c_ulonglong), ("Address", ctypes.c_ulonglong),
        ("Register", wintypes.ULONG), ("Scope", wintypes.ULONG),
        ("Tag", wintypes.ULONG), ("NameLen", wintypes.ULONG),
        ("MaxNameLen", wintypes.ULONG), ("Name", ctypes.c_wchar * 512),
    ]

dbghelp.SymInitializeW(h, r"C:\tjf\github\MiniSys\build\p7", False)
base = dbghelp.SymLoadModuleExW(h, None, r"C:\tjf\github\MiniSys\build\p7\MiniSys.exe",
                                None, 0x10000000, 0, None, 0)
if not base:
    print("SymLoadModuleEx failed:", ctypes.GetLastError()); sys.exit(1)

si = SYMBOL_INFOW()
si.sizeOfStruct = 88
si.MaxNameLen = 511
disp = ctypes.c_ulonglong(0)
ok = dbghelp.SymFromAddrW(h, 0x10000000 + rva, ctypes.byref(disp), ctypes.byref(si))
if ok:
    print(f"SYM: {si.Name} +0x{disp.value:x}")
else:
    print("SymFromAddrW failed:", ctypes.GetLastError())
