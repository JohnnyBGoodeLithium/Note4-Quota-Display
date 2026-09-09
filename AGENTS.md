# 项目维护说明

- 先读 README.md；这是 Note4 墨水屏额度项目，与会议纪要项目无关。
- 安装入口是 `python3 install.py`，依赖 Python 3.11+ 和 systemd 用户服务。
- 不提交账号凭据、录音、设备序列号、环境配置或运行状态。
- 额度均为已用百分比；失败显示未知，不能显示为零。Codex 周窗口可能出现在 primary 或 secondary。
- 主机只经官方额度接口查询；不把 token 写进配置、日志、测试或交接。
- 固件仅为恢复源码，构建和烧录未验证。不要擅自烧录用户正在使用的设备。
- 改动后运行 `python3 -m unittest discover -s tests -v`。测试使用临时 HOME，不调用真实账号和串口。
