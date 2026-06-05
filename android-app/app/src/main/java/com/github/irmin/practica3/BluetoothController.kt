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
            val s = device.createRfcommSocketToServiceRecord(SPP_UUID)
            adapter.cancelDiscovery()
            s.connect()
            socket = s
        }
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
            socket?.outputStream?.write("$command\n".toByteArray(Charsets.US_ASCII))
        }
    }

    fun close() {
        runCatching { socket?.close() }
        socket = null
    }
}