package com.github.irmin.practica3

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.UUID

class BluetoothController(private val adapter: BluetoothAdapter) {

    companion object {
        private val SPP_UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")
    }

    private var socket: BluetoothSocket? = null

    private val _receivedLines = MutableSharedFlow<String>()
    val receivedLines: SharedFlow<String> = _receivedLines

    @SuppressLint("MissingPermission")
    fun pairedDevices(): List<BluetoothDevice> = try {
        adapter.bondedDevices?.toList() ?: emptyList()
    } catch (_: SecurityException) {
        emptyList()
    }

    @SuppressLint("MissingPermission")
    suspend fun connect(device: BluetoothDevice): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            socket?.close()
            socket = null
            try { adapter.cancelDiscovery() } catch (_: SecurityException) {}
            socket = openSocket(device)
        }
    }

    // BT04-A / HC-06 modules often don't advertise SDP, so createRfcommSocketToServiceRecord
    // fails. Try three strategies in order: secure SDP → insecure SDP → reflection channel 1.
    @SuppressLint("MissingPermission")
    private fun openSocket(device: BluetoothDevice): BluetoothSocket {
        try {
            val s = device.createRfcommSocketToServiceRecord(SPP_UUID)
            s.connect()
            return s
        } catch (_: Exception) {}
        try {
            val s = device.createInsecureRfcommSocketToServiceRecord(SPP_UUID)
            s.connect()
            return s
        } catch (_: Exception) {}
        // Direct RFCOMM channel 1 — works for HC-05, HC-06, BT04-A and similar modules
        val method = device.javaClass.getMethod("createRfcommSocket", Int::class.javaPrimitiveType)
        val s = method.invoke(device, 1) as BluetoothSocket
        s.connect()
        return s
    }

    /** Blocks until the socket is closed or an IO error occurs. */
    suspend fun startReceiving() = withContext(Dispatchers.IO) {
        val s = socket ?: return@withContext
        try {
            val reader = BufferedReader(InputStreamReader(s.inputStream, Charsets.US_ASCII))
            while (true) {
                val line = reader.readLine() ?: break
                _receivedLines.emit(line)
            }
        } catch (_: Exception) { }
    }

    suspend fun send(command: String) = withContext(Dispatchers.IO) {
        runCatching {
            socket?.outputStream?.let { out ->
                out.write("$command\n".toByteArray(Charsets.US_ASCII))
                out.flush()
            }
        }
    }

    fun close() {
        runCatching { socket?.close() }
        socket = null
    }
}