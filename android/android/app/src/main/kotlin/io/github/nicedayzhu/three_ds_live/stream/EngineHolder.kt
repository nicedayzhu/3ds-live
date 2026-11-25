package io.github.nicedayzhu.three_ds_live.stream

/** Bridges the foreground service to the activity-owned engine without a binder. */
object EngineHolder {
    @Volatile
    var engine: StreamEngine? = null
}
