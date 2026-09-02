package com.wings.wings_app

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import io.flutter.plugin.common.MethodChannel

/**
 * Idempotent release of HFP/SCO + microphone FGS.
 * Process death skips EventChannel onCancel — always force-stop SCO here.
 * Do not setCommunicationDevice / MODE_IN_COMMUNICATION.
 */
object WingsPark {
    const val TAG = "WingsPark"
    const val ACTION_PARK = "com.wings.wings_app.PARK"
    const val ACTION_DISCONNECT = "com.wings.wings_app.DISCONNECT"
    const val ACTION_RESUME = "com.wings.wings_app.RESUME"

    private val lock = Any()
    private val main = Handler(Looper.getMainLooper())

    @Volatile
    var dartChannel: MethodChannel? = null

    /**
     * @param keepBle demote FGS to connected-device only (Park / Stop listening).
     *        false stops the service (Recents swipe, Disconnect, Dart park).
     * @param tellDart invoke MethodChannel so Flutter can stopAlways / disconnect.
     */
    fun park(ctx: Context, keepBle: Boolean, tellDart: Boolean) {
        synchronized(lock) {
            val app = ctx.applicationContext
            WingsMic.releaseCapture(app)
            if (keepBle) {
                WingsFgService.demoteToConnectedDevice(app)
            } else {
                WingsFgService.stop(app)
            }
        }
        if (tellDart) tell("parked")
    }

    fun disconnect(ctx: Context) {
        park(ctx, keepBle = false, tellDart = false)
        tell("disconnect")
    }

    fun tell(method: String) {
        main.post {
            try {
                dartChannel?.invokeMethod(method, null)
            } catch (e: Exception) {
                Log.w(TAG, "tell $method failed", e)
            }
        }
    }
}
