package local.wechatlite.mobile.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import local.wechatlite.mobile.data.ConnectionConfig

/**
 * First-run screen. It accepts either the whole invitation JSON or the manual fields the desktop
 * client also offers, because the server's "copy client info" dialog produces the JSON form.
 */
@Composable
fun ConnectScreen(
    connecting: Boolean,
    error: String?,
    onConnect: (ConnectionConfig) -> Unit,
    onDismissError: () -> Unit,
) {
    var invitation by remember { mutableStateOf("") }
    var showManual by remember { mutableStateOf(false) }
    var url by remember { mutableStateOf("") }
    var token by remember { mutableStateOf("") }
    var pin by remember { mutableStateOf("") }
    var localError by remember { mutableStateOf<String?>(null) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(24.dp),
        verticalArrangement = Arrangement.Center,
    ) {
        Text("微信轻聊", style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.Bold)
        Spacer(Modifier.height(8.dp))
        Text(
            "连接运行在 Windows 微信电脑上的转发服务端。",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        Spacer(Modifier.height(24.dp))
        OutlinedTextField(
            value = invitation,
            onValueChange = { invitation = it; localError = null },
            label = { Text("连接信息 JSON") },
            placeholder = { Text("在服务端“客户端权限”点击“复制连接信息”后粘贴") },
            modifier = Modifier.fillMaxWidth(),
            minLines = 5,
            maxLines = 10,
            textStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
        )

        Spacer(Modifier.height(12.dp))
        Button(
            onClick = {
                val parsed = ConnectionConfig.parse(invitation)
                if (parsed == null) {
                    localError = "连接信息无效，请复制服务端提供的完整 JSON。"
                } else {
                    onConnect(parsed)
                }
            },
            enabled = !connecting && invitation.isNotBlank(),
            modifier = Modifier.fillMaxWidth(),
        ) {
            if (connecting) {
                CircularProgressIndicator(Modifier.height(18.dp).width(18.dp), strokeWidth = 2.dp)
                Spacer(Modifier.width(8.dp))
            }
            Text(if (connecting) "正在连接…" else "连接")
        }

        Spacer(Modifier.height(8.dp))
        TextButton(onClick = { showManual = !showManual }) {
            Text(if (showManual) "收起手动填写" else "手动填写地址与密钥")
        }

        if (showManual) {
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(
                value = url,
                onValueChange = { url = it },
                label = { Text("服务器地址") },
                placeholder = { Text("https://192.168.1.10:8443") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Next),
            )
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(
                value = token,
                onValueChange = { token = it },
                label = { Text("客户端接入密钥") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, imeAction = ImeAction.Next),
            )
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(
                value = pin,
                onValueChange = { pin = it },
                label = { Text("证书 SHA-256 指纹（可选）") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                textStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
            )
            Spacer(Modifier.height(8.dp))
            Button(
                onClick = {
                    val parsed = ConnectionConfig.parse(
                        org.json.JSONObject()
                            .put("serverUrl", url.trim())
                            .put("token", token.trim())
                            .put("certificatePin", pin.trim())
                            .toString(),
                    )
                    if (parsed == null) localError = "地址或密钥格式不正确。" else onConnect(parsed)
                },
                enabled = !connecting,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("使用手填信息连接") }
        }

        val shown = localError ?: error
        if (shown != null) {
            Spacer(Modifier.height(16.dp))
            Text(
                shown,
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodySmall,
                textAlign = TextAlign.Start,
                modifier = Modifier.fillMaxWidth(),
            )
            TextButton(onClick = onDismissError) { Text("知道了") }
        }

        Spacer(Modifier.height(24.dp))
        Text(
            "密钥只保存在本机应用私有目录，不会写入日志。不要在公网使用管理员密钥。",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
