package com.github.irmin.practica3

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuAnchorType
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import com.github.irmin.practica3.ui.theme.Practica3Theme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            Practica3Theme {
                val vm: MainViewModel = viewModel()
                ControlScreen(vm)
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ControlScreen(vm: MainViewModel) {
    val state by vm.state.collectAsState()
    val context = LocalContext.current

    val permLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted -> if (granted) vm.loadPairedDevices() }

    fun hasBtPermission() = ContextCompat.checkSelfPermission(
        context, Manifest.permission.BLUETOOTH_CONNECT
    ) == PackageManager.PERMISSION_GRANTED

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("PIC16F873A Control") },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.primaryContainer,
                    titleContentColor = MaterialTheme.colorScheme.onPrimaryContainer
                )
            )
        }
    ) { padding ->
        if (!state.btAvailable) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("Bluetooth not available on this device")
            }
            return@Scaffold
        }

        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            ConnectionCard(
                state = state,
                onRequestDevices = {
                    if (hasBtPermission()) vm.loadPairedDevices()
                    else permLauncher.launch(Manifest.permission.BLUETOOTH_CONNECT)
                },
                onConnect = vm::connect,
                onDisconnect = vm::disconnect
            )

            val connected = state.connectionStatus == ConnectionStatus.Connected

            AdcCard(
                adcValues = state.adcValues,
                enabled = connected,
                onRead = vm::requestAdc
            )

            FanCard(
                fanSpeed = state.fanSpeed,
                enabled = connected,
                onSpeedChange = vm::setFanSpeed
            )

            StepperCard(
                stepCount = state.stepCount,
                enabled = connected,
                onStepCountChange = vm::setStepCount,
                onMove = vm::moveStepper
            )
        }
    }
}

@SuppressLint("MissingPermission")
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConnectionCard(
    state: UiState,
    onRequestDevices: () -> Unit,
    onConnect: (BluetoothDevice) -> Unit,
    onDisconnect: () -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    var selectedDevice by remember { mutableStateOf<BluetoothDevice?>(null) }

    ElevatedCard(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text(
                "Bluetooth Connection",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold
            )

            ExposedDropdownMenuBox(
                expanded = expanded,
                onExpandedChange = { open ->
                    if (open) onRequestDevices()
                    expanded = open
                }
            ) {
                OutlinedTextField(
                    value = selectedDevice?.name ?: selectedDevice?.address ?: "Select paired device",
                    onValueChange = {},
                    readOnly = true,
                    singleLine = true,
                    modifier = Modifier
                        .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable)
                        .fillMaxWidth(),
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) }
                )
                ExposedDropdownMenu(
                    expanded = expanded,
                    onDismissRequest = { expanded = false }
                ) {
                    if (state.pairedDevices.isEmpty()) {
                        DropdownMenuItem(
                            text = {
                                Text(
                                    "No paired devices found",
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            },
                            onClick = { expanded = false }
                        )
                    } else {
                        state.pairedDevices.forEach { device ->
                            DropdownMenuItem(
                                text = {
                                    Column {
                                        Text(device.name ?: "Unknown device")
                                        Text(
                                            device.address,
                                            style = MaterialTheme.typography.bodySmall,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant
                                        )
                                    }
                                },
                                onClick = {
                                    selectedDevice = device
                                    expanded = false
                                }
                            )
                        }
                    }
                }
            }

            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                val isConnected = state.connectionStatus == ConnectionStatus.Connected
                val isConnecting = state.connectionStatus == ConnectionStatus.Connecting

                Button(
                    onClick = { if (isConnected) onDisconnect() else selectedDevice?.let(onConnect) },
                    enabled = !isConnecting && (isConnected || selectedDevice != null),
                    modifier = Modifier.width(150.dp)
                ) {
                    if (isConnecting) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(16.dp),
                            strokeWidth = 2.dp,
                            color = MaterialTheme.colorScheme.onPrimary
                        )
                        Spacer(Modifier.width(8.dp))
                        Text("Connecting...")
                    } else {
                        Text(if (isConnected) "Disconnect" else "Connect")
                    }
                }

                StatusBadge(state.connectionStatus)
            }
        }
    }
}

@Composable
fun StatusBadge(status: ConnectionStatus) {
    val (dotColor, label) = when (status) {
        ConnectionStatus.Connected    -> Color(0xFF4CAF50) to "Connected"
        ConnectionStatus.Connecting   -> Color(0xFFFFC107) to "Connecting"
        ConnectionStatus.Error        -> Color(0xFFF44336) to "Error"
        ConnectionStatus.Disconnected -> Color(0xFF9E9E9E) to "Disconnected"
    }
    Surface(
        shape = MaterialTheme.shapes.small,
        color = dotColor.copy(alpha = 0.12f)
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            Box(
                modifier = Modifier
                    .size(8.dp)
                    .background(dotColor, CircleShape)
            )
            Text(label, style = MaterialTheme.typography.labelSmall, color = dotColor)
        }
    }
}

@Composable
fun AdcCard(
    adcValues: List<Int?>,
    enabled: Boolean,
    onRead: () -> Unit
) {
    ElevatedCard(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    "ADC Readings",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )
                Button(onClick = onRead, enabled = enabled) {
                    Text("Read")
                }
            }

            adcValues.forEachIndexed { ch, mv ->
                AdcRow(channel = ch, millivolts = mv, enabled = enabled)
                if (ch < adcValues.lastIndex) Spacer(Modifier.height(4.dp))
            }
        }
    }
}

@Composable
fun AdcRow(channel: Int, millivolts: Int?, enabled: Boolean) {
    val contentAlpha = if (enabled) 1f else 0.4f
    val progress = (millivolts ?: 0) / 5000f

    Column {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Text(
                "AN$channel",
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = contentAlpha)
            )
            Text(
                millivolts?.let { "$it mV" } ?: "--- mV",
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.primary.copy(alpha = contentAlpha)
            )
        }
        Spacer(Modifier.height(4.dp))
        LinearProgressIndicator(
            progress = { progress },
            modifier = Modifier
                .fillMaxWidth()
                .height(6.dp),
            color = MaterialTheme.colorScheme.primary.copy(alpha = contentAlpha),
            trackColor = MaterialTheme.colorScheme.surfaceVariant
        )
    }
}

@Composable
fun FanCard(
    fanSpeed: Int,
    enabled: Boolean,
    onSpeedChange: (Int) -> Unit
) {
    var localSpeed by remember(fanSpeed) { mutableIntStateOf(fanSpeed) }

    ElevatedCard(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    "Fan Control",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    "$localSpeed%",
                    style = MaterialTheme.typography.headlineMedium,
                    color = MaterialTheme.colorScheme.primary
                )
            }

            Slider(
                value = localSpeed.toFloat(),
                onValueChange = { localSpeed = it.toInt() },
                onValueChangeFinished = { if (enabled) onSpeedChange(localSpeed) },
                valueRange = 0f..100f,
                steps = 99,
                enabled = enabled,
                modifier = Modifier.fillMaxWidth()
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text("OFF (0%)", style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text("Full (100%)", style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
fun StepperCard(
    stepCount: Int,
    enabled: Boolean,
    onStepCountChange: (Int) -> Unit,
    onMove: (cw: Boolean) -> Unit
) {
    var stepText by remember { mutableStateOf(stepCount.toString()) }

    ElevatedCard(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text(
                "Stepper Motor",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold
            )

            OutlinedTextField(
                value = stepText,
                onValueChange = { raw ->
                    val digits = raw.filter { it.isDigit() }
                    stepText = digits
                    digits.toIntOrNull()?.let { onStepCountChange(it) }
                },
                label = { Text("Steps per command") },
                supportingText = { Text("Range: 1 – 32 767 full steps") },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                singleLine = true,
                enabled = enabled,
                modifier = Modifier.fillMaxWidth()
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Button(
                    onClick = { onMove(false) },
                    enabled = enabled,
                    modifier = Modifier.weight(1f)
                ) {
                    Text("◄ CCW")
                }
                Button(
                    onClick = { onMove(true) },
                    enabled = enabled,
                    modifier = Modifier.weight(1f)
                ) {
                    Text("CW ►")
                }
            }
        }
    }
}