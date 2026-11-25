package io.github.nicedayzhu.three_ds_live

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import io.github.nicedayzhu.three_ds_live.stream.EngineHolder

/** Keeps the receive engine alive while the player is in the background. */
class StreamService : Service() {
    companion object {
        private const val CHANNEL = "3ds_live_stream"
        private const val NOTIFICATION_ID = 7331
        private const val STOP_ACTION = "io.github.nicedayzhu.three_ds_live.STOP"
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == STOP_ACTION) {
            EngineHolder.engine?.stop()
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
            return START_NOT_STICKY
        }
        startForeground(NOTIFICATION_ID, notification())
        return START_STICKY
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = getSystemService(NotificationManager::class.java)
        val channel = NotificationChannel(
            CHANNEL,
            "3DS Live 投流",
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = "保持掌机画面持续接收"
            setShowBadge(false)
        }
        manager.createNotificationChannel(channel)
    }

    private fun notification(): Notification {
        val stop = PendingIntent.getService(
            this,
            1,
            Intent(this, StreamService::class.java).setAction(STOP_ACTION),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        return builder
            .setContentTitle("3DS Live 正在投流")
            .setContentText("掌机画面持续接收中，点按返回播放器")
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setOngoing(true)
            .setContentIntent(
                PendingIntent.getActivity(
                    this,
                    2,
                    Intent(this, MainActivity::class.java),
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
                ),
            )
            .addAction(
                Notification.Action.Builder(
                    null,
                    getString(android.R.string.cancel),
                    stop,
                ).build(),
            )
            .build()
    }
}
