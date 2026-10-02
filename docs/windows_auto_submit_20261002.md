# Windows 10 识别成功后仍需 Enter（2026-10-02）

## 原因与改动

Windows 10 19044 测试机既有 `C:\Windows\Temp\su_cp.log` 记录了以下顺序：

1. 识别管道返回 `status=0`。
2. LogonUI 调用卡片 `Credential::UnAdvise`，重新枚举卡片。
3. `GetCredentialCount` 返回 `autologon=false`，没有自动提交。
4. 后续手动触发 `GetSerialization`，再次识别后返回凭据，Windows 登录成功。

该机注册的 `C:\Program Files\Smile2Unlock\bin\su_credential_provider.dll`
SHA-256 为 `6f59e37cb02c674957525f814777f80b05038dfcd08de4c6d8ebbfcd7618ac57`，
已安装 manifest 版本 2.3.0。日志里的旧实现每次枚举新建卡片；主线已有 SID 缓存，
但卡片 `UnAdvise` 仍然取消同一 owner 的识别，清掉刚准备好的凭据，缺陷依然存在。

现在将卡片 `UnAdvise` 限定为断开字段事件连接。真正的取消选择、卡片销毁、
用户数组替换和 provider `UnAdvise` 仍会取消识别并清除凭据。卡片
`SetSelected` 在本 owner/SID 的未过期凭据就绪时返回 `TRUE`，准备期间保持
`FALSE`，查询本身不消费凭据。密码框空到空的初始化/刷新不再被当成输入密码；
输入密码或清空已输入的密码仍会取消识别。

微软说明 `CredentialsChanged` 用于异步自动登录时的重新枚举，
[CredentialsChanged 文档](https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nf-credentialprovider-icredentialproviderevents-credentialschanged)；
选择回调的自动提交标志见
[SetSelected 文档](https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nf-credentialprovider-icredentialprovidercredential-setselected)。

## 验证与边界

使用 Rust 1.98.1；仅在现有 CP 套件中增加两个针对本缺陷的回归用例，没有修改 CI 工作流。

- 完整 Windows-target CP 套件在 Wine 下：91 passed，1 个已有忽略项。
- 同一两个新增用例在原生 Windows 10 19044 执行：2/2 passed。通过真实 COM
  接口验证 unadvise → provider 枚举 → 同一卡片 advise/重选，两个自动提交标志
  都保持有效；就绪查询不消费凭据，消费只允许一次，真正取消选择及 provider
  关闭仍会清除凭据。另一个用例覆盖空字段刷新及实际密码编辑的区别。
- 对照副本只恢复这三个旧回调，保留相同的新测试/fixture：两个新增用例失败，
  分别检出自动登录标志被清除和空字段取消识别。
- 过期凭据、错误 owner/SID 的就绪查询由现有有效期用例验证；Release DLL 构建通过。

原始日志、对照副本和独立测试 EXE 保留在忽略目录
`build/windows-auto-submit-20261002/`。原生测试 EXE 只放入测试机临时目录；
没有替换其注册 DLL、重启 LogonUI 或改动现有登录配置。

这里证明的是 Windows COM 回调契约，尚未将修复 DLL 部署进真正的 LogonUI
验证完整无按键解锁。真人识别与摄像头也没有在本次重复测试。
本次检查时，远端 `v2.3.1` tag 仍为 `c64f02b85f1b58eff264187c04be602d4043c94c`，
不含后续主线修复及本次改动。用户端生效需要重新构建并发布完整签名包；
本次没有移动发布 tag 或重新发布安装器。
