# Note4 Quota Display · 墨水屏额度看板

把 Codex 周额度、Kimi Code 周额度和 5 小时额度显示到 Zectrix Note4 墨水屏上。展示的是**已用百分比**及重置时间；查询失败显示 `--`，不会把失败当成还有额度。

Linux 主机通过 USB 更新屏幕，默认两分钟查询一次，断线自动重连。Python 主机程序只使用标准库。

## 安装

需要 Linux、systemd 用户服务、Python 3.11+，以及已运行兼容固件的 Note4。当前恢复验证的设备是 Espressif 平台，并非 STM32；不适用于任意 USB 墨水屏。

1. 在本机安装并登录 Codex CLI、Kimi Code。只使用其中一个时，另一项显示 `--`。
2. 下载本项目，在项目目录运行 `python3 install.py`。服务随用户登录启动。
3. 插入设备。只有一台匹配的 Espressif USB 串口时自动发现；多设备时在 `~/.config/note4-quota-display/environment` 设置 `NOTE4_DEVICE=/dev/serial/by-id/实际设备路径`。
4. 修改配置后运行 `systemctl --user restart note4-quota-display.service note4-quota-refresh.service`。

安装脚本将程序复制到 `~/.local/share/note4-quota-display`，可删除下载目录；更新时再次运行安装脚本，已有配置保留。代理可在上述配置中设置 `HTTPS_PROXY`。串口必须对当前用户可读写；权限不足时按发行版的串口用户组规则设置后重新登录。不要与其他串口服务同时占用同一设备。

## 检查与卸载

```bash
systemctl --user status note4-quota-display.service note4-quota-refresh.timer
journalctl --user -u note4-quota-display.service -u note4-quota-refresh.service -n 40
cat ~/.local/state/note4-quota-display/usage.json
```

`health.json` 记录 USB 状态，`usage.json` 记录查询状态。`partial` 表示一个来源失败。Kimi 登录过期时重新在其客户端登录；本程序不管理令牌刷新。

卸载时停止并禁用服务，再移除对应单元、安装目录；个人配置和运行数据可按需保留：

```bash
systemctl --user disable --now note4-quota-refresh.timer note4-quota-display.service
systemctl --user stop note4-quota-refresh.service
rm ~/.config/systemd/user/note4-quota-display.service ~/.config/systemd/user/note4-quota-refresh.service ~/.config/systemd/user/note4-quota-refresh.timer
systemctl --user daemon-reload
rm -r ~/.local/share/note4-quota-display
```

## 数据与接口

Codex 通过本机 `codex app-server` 的 `account/rateLimits/read` 获取额度。Kimi 读取本机 `~/.kimi-code/credentials/kimi-code.json`，仅向 `https://api.kimi.com/coding/v1/usages` 发起认证查询。接口或客户端凭据格式变化可能需要适配。

仓库不包含登录凭据、Wi-Fi 配置、个人设备序列号或录音。默认丢弃板端上传音频；明确设置 `NOTE4_SAVE_AUDIO=1` 才会保存到本机状态目录。此项目不执行语音指令。

## 固件和验证范围

`firmware/src/main.cpp` 是从原交接恢复的参考源码，包含显示、按键、音频和无线功能。原构建配置和固件二进制缺失，**尚未重建或验证烧录流程**。安装脚本只部署主机服务；不能据此给空白开发板一键烧录。BLE UUID 保留协议兼容值，设备广播名称已通用化。

主机服务原版本已在一台 Note4 上验证额度查询和 USB 回执。本仓库通用化版本使用离线测试验证解析和安装；尚未在第二台机器验证。运行测试：

```bash
python3 -m unittest discover -s tests -v
```
