package com.facebook.proguard.annotations

/** Host/fixture annotation signature only; React Native supplies the release keep rule. */
@Target(AnnotationTarget.FIELD)
@Retention(AnnotationRetention.BINARY)
annotation class DoNotStrip
