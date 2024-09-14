// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use std::{mem::forget, ptr::NonNull};

/// Rust wrapper around `native_handle_t`.
///
/// This owns the `native_handle_t` and its file descriptors, and will close them and free it when
/// it is dropped.
#[derive(Debug)]
pub struct NativeHandle(NonNull<ffi::native_handle_t>);

impl NativeHandle {
    /// Wraps a raw `native_handle_t` pointer, taking ownership of it.
    ///
    /// # Safety
    ///
    /// `native_handle` must be a valid pointer to a `native_handle_t`, and must not be used
    ///  anywhere else after calling this method.
    pub unsafe fn from_raw(native_handle: NonNull<ffi::native_handle_t>) -> Self {
        Self(native_handle)
    }

    /// Creates a new `NativeHandle` wrapping a clone of the given `native_handle_t` pointer.
    ///
    /// Unlike [`from_raw`](Self::from_raw) this doesn't take ownership of the pointer passed in, so
    /// the caller remains responsible for closing and freeing it.
    ///
    /// # Safety
    ///
    /// `native_handle` must be a valid pointer to a `native_handle_t`.
    pub unsafe fn clone_from_raw(native_handle: NonNull<ffi::native_handle_t>) -> Option<Self> {
        // SAFETY: The caller promised that `native_handle` was valid.
        let cloned = unsafe { ffi::native_handle_clone(native_handle.as_ptr()) };
        NonNull::new(cloned).map(Self)
    }

    /// Returns a raw pointer to the wrapped `native_handle_t`.
    ///
    /// This is only valid as long as this `NativeHandle` exists, so shouldn't be stored. It mustn't
    /// be closed or deleted.
    pub fn as_raw(&self) -> NonNull<ffi::native_handle_t> {
        self.0
    }

    /// Turns the `NativeHandle` into a raw `native_handle_t`.
    ///
    /// The caller takes ownership of the `native_handle_t` and its file descriptors, so is
    /// responsible for closing and freeing it.
    pub fn into_raw(self) -> NonNull<ffi::native_handle_t> {
        let raw = self.0;
        forget(self);
        raw
    }
}

impl Clone for NativeHandle {
    fn clone(&self) -> Self {
        // SAFETY: Our wrapped `native_handle_t` pointer is always valid.
        unsafe { Self::clone_from_raw(self.0) }.expect("native_handle_clone returned null")
    }
}

impl Drop for NativeHandle {
    fn drop(&mut self) {
        // SAFETY: Our wrapped `native_handle_t` pointer is always valid, and it won't be accessed
        // after this because we own it and are being dropped.
        unsafe {
            assert_eq!(ffi::native_handle_close(self.0.as_ptr()), 0);
            assert_eq!(ffi::native_handle_delete(self.0.as_ptr()), 0);
        }
    }
}

// SAFETY: `NativeHandle` owns the `native_handle_t`, which just contains some integers and file
// descriptors, which aren't tied to any particular thread.
unsafe impl Send for NativeHandle {}

// SAFETY: A `NativeHandle` can be used from different threads simultaneously, as is is just
// integers and file descriptors.
unsafe impl Sync for NativeHandle {}
