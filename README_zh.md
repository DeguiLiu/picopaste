[English](README.md) | [中文](README_zh.md)

# picopaste

将该操作压缩为一次热键：截图进入剪贴板后按下 `alt+shift+v`，图片即上传至 Linux，
远端的绝对路径随即被写入剪贴板并输入到当前焦点窗口。Claude Code 依据该路径即可读取图片，
无需任何后续操作，路径已位于光标位置。

以 Codex CLI 为主要目标，Claude Code 使用同一路径同样可读；远端只需系统自带的
`sshd` 与其 `sftp-server`。

## 开始使用

获取 release 中的 `picopaste.exe`（或自行构建），在**其同级目录**放置 `picopaste.ini`：

```ini
[picopaste]
host = user@your-linux-host
hotkey = alt+shift+v
```

随后运行。托盘图标出现并立即开始连接：最多短暂显示黄色，连接成功后为绿色。
此后截取图片并按下热键即可。

## 安装目录

安装目录必须是纯英文路径，例如 `C:\tools\picopaste`，不要使用中文目录；
`picopaste.exe`、`picopaste.ini` 与 `README.md` 需位于同一目录。

## 前置条件与排障

Windows 必须能免密登录 Linux 主机，先在 cmd 中手工验证：

```text
ssh user@your-linux-host
```

运行 `picopaste.exe --selftest` 可自检剪贴板、图片编码、热键、内存与 SFTP。

| 现象 | 处理 |
|---|---|
| 提示 `host` 为空 | 确认 INI 与 exe 同目录并填写 `host` |
| 配置未生效 | 将目录改为纯英文路径 |
| 托盘一直黄色或变红 | 先验证免密 ssh 可登录 |
| 按热键没有反应 | 更换 `hotkey`，例如 `ctrl+alt+p` |
| 大截图失败 | 调高 `job_memory_limit_mb` |

## 构建与源码

```bash
cmake -S . -B build -DPICOPASTE_BUILD_TESTS=ON \
  -DPICOPASTE_NEWOSP_DIR=$HOME/newosp-windows
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

- GitLab：http://172.16.40.222/dgliu/picopaste
- GitHub：https://github.com/DeguiLiu/picopaste
