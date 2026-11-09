#!/usr/bin/env bash
# 查询这台机器可用的 OpenVINO 推理设备（不涉及相机）
echo "=== OpenVINO 可用设备 ==="
python3 - <<'PYEOF' 2>/dev/null || echo "  python openvino 未安装，用别的方式查"
try:
    import openvino as ov
    core = ov.Core()
    print("  OpenVINO 版本:", core.get_versions("CPU").get("CPU", {}).get("buildNumber", "?"))
    print("  可用设备:")
    for d in core.available_devices:
        try:
            print("    -", d, ":", core.get_property(d, "FULL_DEVICE_NAME"))
        except Exception as e:
            print("    -", d, "(取名称失败)")
except Exception as e:
    print("  失败:", e)
PYEOF

echo
echo "=== 已安装的 OpenVINO 插件包 ==="
dpkg -l 2>/dev/null | grep -i openvino | awk '{print "  " $2}' | head -20

echo
echo "=== 插件 .so 文件 ==="
ls /usr/lib/x86_64-linux-gnu/libopenvino_*.so 2>/dev/null | sed 's/^/  /' | head -20
