/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.settings.widget;

import android.os.SystemClock;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;
import androidx.recyclerview.widget.RecyclerView;
import java.util.function.BooleanSupplier;

/** Presents the first preference list as one layout, without animating initial insertions. */
public final class PreferenceInitialDraw implements ViewTreeObserver.OnPreDrawListener,
        View.OnAttachStateChangeListener {
    static final long MAX_WAIT_MS = 250;
    private static final String TAG = "SettingsFirstDraw";
    private static final boolean DEBUG = Log.isLoggable(TAG, Log.DEBUG);
    private final View mView;
    private final RecyclerView mList;
    private final BooleanSupplier mReady;
    private final RecyclerView.ItemAnimator mAnimator;
    private long mFirstDrawTime = -1;
    private boolean mReadyOnPreviousFrame;
    private boolean mFinished;

    public PreferenceInitialDraw(View view, RecyclerView list, BooleanSupplier ready) {
        mView = view;
        mList = list;
        mReady = ready;
        mAnimator = list.getItemAnimator();
        list.setItemAnimator(null);
        view.getViewTreeObserver().addOnPreDrawListener(this);
        view.addOnAttachStateChangeListener(this);
    }

    @Override public boolean onPreDraw() {
        final long now = SystemClock.uptimeMillis();
        // Inflation and the activity transition must not consume the asynchronous settling budget.
        if (mFirstDrawTime < 0) mFirstDrawTime = now;
        final boolean ready = mReady.getAsBoolean() && !mList.hasPendingAdapterUpdates();
        final boolean expired = now - mFirstDrawTime >= MAX_WAIT_MS;
        if (DEBUG) Log.d(TAG, "ready=" + ready + " settled=" + mReadyOnPreviousFrame
                + " elapsed=" + (now - mFirstDrawTime) + " expired=" + expired);
        if (!expired && !(ready && mReadyOnPreviousFrame)) {
            // A hierarchy callback posts adapter synchronization. Give it a traversal to settle.
            mReadyOnPreviousFrame = ready;
            mView.postInvalidateOnAnimation();
            return false;
        }
        finish();
        return true;
    }

    private void finish() {
        if (mFinished) return;
        mFinished = true;
        if (mView.getViewTreeObserver().isAlive()) {
            mView.getViewTreeObserver().removeOnPreDrawListener(this);
        }
        mView.removeOnAttachStateChangeListener(this);
        // Restore only our own temporary override; respect screens that replace the animator.
        if (mList.getItemAnimator() == null) mList.setItemAnimator(mAnimator);
    }

    @Override public void onViewAttachedToWindow(View view) {}
    @Override public void onViewDetachedFromWindow(View view) { finish(); }
}
