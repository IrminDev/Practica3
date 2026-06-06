package com.github.irmin.practica3

import android.annotation.SuppressLint
import android.app.Application
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

enum class ConnectionStatus { Disconnected, Connecting, Connected, Error }

data class UiState(
    val btAvailable: Boolean = true,
    val connectionStatus: ConnectionStatus = ConnectionStatus.Disconnected,
    val pairedDevices: List<BluetoothDevice> = emptyList(),
    val adcValues: List<Int?> = listOf(null, null, null, null),
    val fanSpeed: Int = 0,
    val stepCount: Int = 10,
    val errorMessage: String? = null,
)

class MainViewModel(app: Application) : AndroidViewModel(app) {

    private val adapter: BluetoothAdapter? =
        app.getSystemService(BluetoothManager::class.java)?.adapter

    private val controller: BluetoothController? = adapter?.let { BluetoothController(it) }

    private val _state = MutableStateFlow(UiState(btAvailable = adapter != null))
    val state: StateFlow<UiState> = _state.asStateFlow()

    private var receiveJob: Job? = null

    @SuppressLint("MissingPermission")
    fun loadPairedDevices() {
        _state.update { it.copy(pairedDevices = controller?.pairedDevices() ?: emptyList()) }
    }

    fun connect(device: BluetoothDevice) {
        val c = controller ?: return
        _state.update { it.copy(connectionStatus = ConnectionStatus.Connecting) }
        viewModelScope.launch {
            val result = c.connect(device)
            if (result.isSuccess) {
                _state.update { it.copy(connectionStatus = ConnectionStatus.Connected, errorMessage = null) }
                startReceiving(c)
            } else {
                val msg = result.exceptionOrNull()?.let { "${it.javaClass.simpleName}: ${it.message}" }
                _state.update { it.copy(connectionStatus = ConnectionStatus.Error, errorMessage = msg) }
            }
        }
    }

    fun disconnect() {
        receiveJob?.cancel()
        controller?.close()
        _state.update { it.copy(connectionStatus = ConnectionStatus.Disconnected) }
    }

    private fun startReceiving(c: BluetoothController) {
        receiveJob?.cancel()
        receiveJob = viewModelScope.launch {
            val collectJob = launch {
                c.receivedLines.collect { line -> parseAdcResponse(line) }
            }
            c.startReceiving()   // suspends until socket drops
            collectJob.cancel()
            if (_state.value.connectionStatus == ConnectionStatus.Connected) {
                _state.update { it.copy(connectionStatus = ConnectionStatus.Disconnected) }
            }
        }
    }

    // Parses "V0:<mV>,V1:<mV>,V2:<mV>,V3:<mV>" sent by the PIC after an "R" command.
    private fun parseAdcResponse(line: String) {
        runCatching {
            val parts = line.split(",")
            if (parts.size == 4) {
                val values: List<Int?> = parts.map { it.substringAfter(":").trim().toInt() }
                _state.update { it.copy(adcValues = values) }
            }
        }
    }

    fun requestAdc() {
        viewModelScope.launch { controller?.send("R") }
    }

    fun setFanSpeed(pct: Int) {
        val clamped = pct.coerceIn(0, 100)
        _state.update { it.copy(fanSpeed = clamped) }
        viewModelScope.launch { controller?.send("F$clamped") }
    }

    fun setStepCount(n: Int) {
        _state.update { it.copy(stepCount = n.coerceIn(1, 32767)) }
    }

    fun moveStepper(cw: Boolean) {
        val n = _state.value.stepCount
        val cmd = if (cw) "M+$n" else "M-$n"
        viewModelScope.launch { controller?.send(cmd) }
    }

    override fun onCleared() {
        super.onCleared()
        controller?.close()
    }
}