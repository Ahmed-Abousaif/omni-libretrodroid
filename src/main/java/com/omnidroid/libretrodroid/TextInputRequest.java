package com.omnidroid.libretrodroid;

/** A software-keyboard request issued by the running core. Text uses UTF-16. */
public final class TextInputRequest {
    public final long session;
    public final String text;
    public final int maxLength;

    public TextInputRequest(long session, String text, int maxLength) {
        this.session = session;
        this.text = text;
        this.maxLength = maxLength;
    }
}
