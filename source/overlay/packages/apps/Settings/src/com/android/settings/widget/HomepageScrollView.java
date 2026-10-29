/*
 * Copyright (C) 2026 The Android Open Source Project
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.android.settings.widget;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;
import android.view.ViewGroup;

import androidx.core.widget.NestedScrollView;

import com.android.settings.R;
import com.android.settings.flags.Flags;

/** Keeps the home preference list virtualized when there are no cards above it. */
public class HomepageScrollView extends NestedScrollView {
    private boolean mBoundedContent;

    public HomepageScrollView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        mBoundedContent = MeasureSpec.getMode(heightMeasureSpec) != MeasureSpec.UNSPECIFIED
                && !hasHeaderContent(R.id.suggestion_content)
                && !hasHeaderContent(R.id.contextual_cards_content);
        super.onMeasure(widthMeasureSpec, heightMeasureSpec);
        if (mBoundedContent) {
            // The RecyclerView now owns scrolling and extended screenshots.
            setScrollCaptureHint(View.SCROLL_CAPTURE_HINT_AUTO);
        } else if (Flags.extendedScreenshotsExcludeNestedScrollables()) {
            setScrollCaptureHint(View.SCROLL_CAPTURE_HINT_EXCLUDE_DESCENDANTS);
        }
    }

    private boolean hasHeaderContent(int id) {
        View header = findViewById(id);
        return header != null && header.getVisibility() != View.GONE
                && (!(header instanceof ViewGroup) || ((ViewGroup) header).getChildCount() > 0);
    }

    @Override
    protected void measureChildWithMargins(View child, int parentWidthMeasureSpec, int widthUsed,
            int parentHeightMeasureSpec, int heightUsed) {
        if (!mBoundedContent) {
            // Preserve the original shared scrolling behavior when suggestions/cards are present.
            super.measureChildWithMargins(child, parentWidthMeasureSpec, widthUsed,
                    parentHeightMeasureSpec, heightUsed);
            return;
        }
        MarginLayoutParams lp = (MarginLayoutParams) child.getLayoutParams();
        int widthSpec = getChildMeasureSpec(parentWidthMeasureSpec,
                getPaddingLeft() + getPaddingRight() + lp.leftMargin + lp.rightMargin + widthUsed,
                lp.width);
        int heightSpec = getChildMeasureSpec(parentHeightMeasureSpec,
                getPaddingTop() + getPaddingBottom() + lp.topMargin + lp.bottomMargin + heightUsed,
                lp.height);
        child.measure(widthSpec, heightSpec);
    }
}
