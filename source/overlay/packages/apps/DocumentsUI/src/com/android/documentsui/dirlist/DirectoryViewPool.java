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
package com.android.documentsui.dirlist;

import android.content.res.Configuration;

import androidx.recyclerview.widget.RecyclerView;

import com.android.documentsui.base.State;

/** A bounded, activity-owned pool; never retain item views across activity recreation. */
public final class DirectoryViewPool {
    private RecyclerView.RecycledViewPool mPool;
    private Configuration mConfiguration;
    private int mMode;
    private boolean mPhotoPicking;
    private boolean mCrossProfile;

    public RecyclerView.RecycledViewPool obtain(State state, Configuration configuration) {
        if (mPool == null || mMode != state.derivedMode
                || mPhotoPicking != state.isPhotoPicking()
                || mCrossProfile != state.supportsCrossProfile()
                || !configuration.equals(mConfiguration)) {
            mPool = new RecyclerView.RecycledViewPool();
            // Enough for the visible grid and nearby rows, with a fixed upper bound.
            mPool.setMaxRecycledViews(DocumentsAdapter.ITEM_TYPE_DOCUMENT, 24);
            mPool.setMaxRecycledViews(DocumentsAdapter.ITEM_TYPE_DIRECTORY, 24);
            // Message holders can retain directory-specific callbacks; do not share them.
            mPool.setMaxRecycledViews(DocumentsAdapter.ITEM_TYPE_SECTION_BREAK, 0);
            mPool.setMaxRecycledViews(DocumentsAdapter.ITEM_TYPE_HEADER_MESSAGE, 0);
            mPool.setMaxRecycledViews(DocumentsAdapter.ITEM_TYPE_INFLATED_MESSAGE, 0);
            mMode = state.derivedMode;
            mPhotoPicking = state.isPhotoPicking();
            mCrossProfile = state.supportsCrossProfile();
            mConfiguration = new Configuration(configuration);
        }
        return mPool;
    }
}
