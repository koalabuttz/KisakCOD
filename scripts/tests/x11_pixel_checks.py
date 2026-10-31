"""Read actual X11 drawable pixels; no SDL event injection or renderer doubles."""

import ctypes
import ctypes.util


def read_quadrants(display, window):
    # Prefix of the public XImage structure through its RGB masks. XGetPixel
    # handles image byte order/stride; masks map the visual to RGB channels.
    class Image(ctypes.Structure):
        _fields_ = [("width", ctypes.c_int), ("height", ctypes.c_int),
                    ("xoffset", ctypes.c_int), ("format", ctypes.c_int),
                    ("data", ctypes.c_void_p), ("byte_order", ctypes.c_int),
                    ("bitmap_unit", ctypes.c_int), ("bitmap_bit_order", ctypes.c_int),
                    ("bitmap_pad", ctypes.c_int), ("depth", ctypes.c_int),
                    ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int),
                    ("red_mask", ctypes.c_ulong), ("green_mask", ctypes.c_ulong),
                    ("blue_mask", ctypes.c_ulong)]

    library = ctypes.util.find_library("X11")
    if not library:
        raise RuntimeError("host libX11 is required for visual readback")
    x11 = ctypes.CDLL(library)
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    x11.XGetGeometry.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
                                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint),
                                ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
                                ctypes.POINTER(ctypes.c_uint)]
    x11.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                             ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
    x11.XGetImage.restype = ctypes.POINTER(Image)
    x11.XGetPixel.argtypes = [ctypes.POINTER(Image), ctypes.c_int, ctypes.c_int]
    x11.XGetPixel.restype = ctypes.c_ulong
    x11.XDestroyImage.argtypes = [ctypes.POINTER(Image)]
    connection = x11.XOpenDisplay(display.encode())
    if not connection:
        raise RuntimeError("could not open private X display for readback")
    try:
        root, x, y = ctypes.c_ulong(), ctypes.c_int(), ctypes.c_int()
        width, height, border, depth = (ctypes.c_uint() for _ in range(4))
        if not x11.XGetGeometry(connection, window, ctypes.byref(root), ctypes.byref(x), ctypes.byref(y),
                                ctypes.byref(width), ctypes.byref(height), ctypes.byref(border), ctypes.byref(depth)):
            raise RuntimeError("XGetGeometry failed for presentation window")
        result = {"width": width.value, "height": height.value, "depth": depth.value, "samples": {}}
        positions = (("top_left", width.value // 4, height.value // 4),
                     ("top_right", 3 * width.value // 4, height.value // 4),
                     ("bottom_left", width.value // 4, 3 * height.value // 4),
                     ("bottom_right", 3 * width.value // 4, 3 * height.value // 4))
        for label, px, py in positions:
            image = x11.XGetImage(connection, window, px, py, 1, 1, ctypes.c_ulong(-1).value, 2)  # ZPixmap
            if not image:
                raise RuntimeError(f"XGetImage failed at ({px}, {py})")
            try:
                pixel = x11.XGetPixel(image, 0, 0)
                rgb = []
                for mask in (image.contents.red_mask, image.contents.green_mask, image.contents.blue_mask):
                    if not mask:
                        raise RuntimeError("X11 visual has no direct RGB mask")
                    shift = (mask & -mask).bit_length() - 1
                    rgb.append(((pixel & mask) >> shift) * 255 // (mask >> shift))
                result["samples"][label] = {"x": px, "y": py, "rgb": rgb}
            finally:
                x11.XDestroyImage(image)
        return result
    finally:
        x11.XCloseDisplay(connection)
