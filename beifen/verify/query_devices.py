import openvino as ov
c = ov.Core()
print('OpenVINO', ov.__version__)
try:
    print('available_devices =', c.available_devices)
except Exception as e:
    print('available_devices err:', e)
for d in ['CPU', 'GPU', 'NPU', 'AUTO', 'MULTI', 'HETERO']:
    try:
        print('  %-8s -> %s' % (d, c.get_property(d, 'FULL_DEVICE_NAME')))
    except Exception as e:
        print('  %-8s -> 不可用 (%s)' % (d, str(e).splitlines()[0][:90]))
