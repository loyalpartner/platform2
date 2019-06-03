// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::cmp::min;
use std::fmt;
use std::mem::size_of;
use std::result::Result;

use rand_core::{Error, ErrorKind, RngCore};

/// A random number generator that uses fuzzer input as the source of its
/// randomness.  When run on the same input, it provides the same output, as
/// long as its methods are called in the same order and with the same
/// arguments.
pub struct FuzzRng<'a> {
    buf: &'a [u8],
}

impl<'a> FuzzRng<'a> {
    /// Creates a new `FuzzRng` from `buf`, which should be part or all of an
    /// input buffer provided by a fuzzing library.
    pub fn new(buf: &'a [u8]) -> FuzzRng<'a> {
        FuzzRng { buf: buf }
    }
}

impl<'a> RngCore for FuzzRng<'a> {
    fn next_u32(&mut self) -> u32 {
        let mut buf = [0u8; size_of::<u32>()];
        self.fill_bytes(&mut buf);

        u32::from_ne_bytes(buf)
    }

    fn next_u64(&mut self) -> u64 {
        let mut buf = [0u8; size_of::<u64>()];
        self.fill_bytes(&mut buf);

        u64::from_ne_bytes(buf)
    }

    fn fill_bytes(&mut self, dest: &mut [u8]) {
        let amt = min(self.buf.len(), dest.len());
        let (a, b) = self.buf.split_at(amt);
        dest[..amt].copy_from_slice(a);
        self.buf = b;

        if amt < dest.len() {
            // We didn't have enough data to fill the whole buffer.  Fill the rest
            // with zeroes.  The compiler is smart enough to turn this into a memset.
            for b in &mut dest[amt..] {
                *b = 0;
            }
        }
    }

    fn try_fill_bytes(&mut self, dest: &mut [u8]) -> Result<(), Error> {
        if self.buf.len() >= dest.len() {
            Ok(self.fill_bytes(dest))
        } else {
            Err(Error::new(
                ErrorKind::Unavailable,
                "not enough data in fuzzer input",
            ))
        }
    }
}

impl<'a> fmt::Debug for FuzzRng<'a> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "FuzzRng {{ {} bytes }}", self.buf.len())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gen_u32() {
        let val = 0xc2a744u32;
        let buf = val.to_ne_bytes();
        let mut rng = FuzzRng::new(&buf);

        assert_eq!(rng.next_u32(), val);
        assert_eq!(rng.next_u32(), 0);
    }

    #[test]
    fn gen_u64() {
        let val = 0xac75689deeu64;
        let buf = val.to_ne_bytes();
        let mut rng = FuzzRng::new(&buf);

        assert_eq!(rng.next_u64(), val);
        assert_eq!(rng.next_u64(), 0);
    }

    #[test]
    fn fill_bytes() {
        let buf = &[
            0xed, 0x90, 0xf3, 0xa4, 0x8f, 0xbf, 0x6e, 0xdb, 0x68, 0xb9, 0x1f, 0x9a, 0x13, 0xfc,
            0x9f, 0xc8, 0x9e, 0xfa, 0x4a, 0x02, 0x5e, 0xc8, 0xb1, 0xe5, 0x2d, 0x59, 0x22, 0x89,
            0x10, 0x23, 0xc3, 0x31, 0x6c, 0x42, 0x40, 0xce, 0xfe, 0x6e, 0x5c, 0x3d, 0x10, 0xba,
            0x0d, 0x11, 0xbc, 0x6a, 0x1f, 0x21, 0xc9, 0x72, 0x37, 0xba, 0xfa, 0x00, 0xb2, 0xa8,
            0x51, 0x6d, 0xb2, 0x94, 0xf2, 0x34, 0xf8, 0x3c, 0x21, 0xc9, 0x59, 0x24, 0xd8, 0x77,
            0x51, 0x3f, 0x64, 0xde, 0x19, 0xc8, 0xb3, 0x03, 0x26, 0x81, 0x85, 0x4c, 0xef, 0xb0,
            0xd5, 0xd8, 0x65, 0xe1, 0x89, 0x8f, 0xb7, 0x14, 0x9b, 0x0d, 0xd9, 0xcb, 0xda, 0x35,
            0xb2, 0xff, 0xd5, 0xd1, 0xae, 0x38, 0x55, 0xd5, 0x65, 0xba, 0xdc, 0xa1, 0x82, 0x62,
            0xbf, 0xe6, 0x3d, 0x7a, 0x8f, 0x13, 0x65, 0x2f, 0x4b, 0xdc, 0xcb, 0xee, 0xd8, 0x99,
            0x2c, 0x21, 0x97, 0xc8, 0x6e, 0x8e, 0x09, 0x0f, 0xf1, 0x4b, 0x85, 0xb5, 0x0f, 0x52,
            0x82, 0x7f, 0xe0, 0x23, 0xc5, 0x9a, 0x6a, 0x7c, 0xf1, 0x46, 0x7d, 0xbf, 0x3f, 0x14,
            0x0d, 0x41, 0x09, 0xd5, 0x63, 0x70, 0xa1, 0x0e, 0x04, 0x3c, 0x06, 0x0a, 0x0b, 0x5c,
            0x95, 0xaf, 0xbd, 0xf5, 0x4b, 0x7f, 0xbe, 0x8d, 0xe2, 0x09, 0xce, 0xa2, 0xf6, 0x1e,
            0x58, 0xd8, 0xda, 0xd4, 0x56, 0x56, 0xe1, 0x32, 0x30, 0xef, 0x0f, 0x2e, 0xed, 0xb9,
            0x14, 0x57, 0xa8, 0x8a, 0x9c, 0xd8, 0x58, 0x7f, 0xd9, 0x4f, 0x11, 0xb2, 0x7a, 0xcf,
            0xc0, 0xef, 0xf3, 0xc7, 0xc1, 0xc5, 0x1e, 0x86, 0x47, 0xc6, 0x42, 0x71, 0x15, 0xc8,
            0x25, 0x1d, 0x94, 0x00, 0x8d, 0x04, 0x37, 0xe7, 0xfe, 0xf6, 0x10, 0x28, 0xe5, 0xb2,
            0xef, 0x95, 0xa6, 0x53, 0x20, 0xf8, 0x51, 0xdb, 0x54, 0x99, 0x40, 0x4a, 0x7c, 0xd6,
            0x90, 0x4a, 0x55, 0xdc, 0x37, 0xb8, 0xbc, 0x0b, 0xc4, 0x54, 0xd1, 0x9b, 0xb3, 0x8c,
            0x09, 0x55, 0x77, 0xf5, 0x1b, 0xa7, 0x36, 0x06, 0x29, 0x4c, 0xa3, 0x26, 0x35, 0x1b,
            0x29, 0xa3, 0xa3, 0x45, 0x74, 0xee, 0x0b, 0x78, 0xf8, 0x69, 0x70, 0xa4, 0x1d, 0x11,
            0x7a, 0x91, 0xca, 0x4c, 0x83, 0xb3, 0xbf, 0xf6, 0x7f, 0x54, 0xca, 0xdb, 0x1f, 0xc4,
            0xd2, 0xb2, 0x23, 0xfa, 0xc0, 0x24, 0x77, 0x74, 0x61, 0x9e, 0x0b, 0x77, 0x49, 0x29,
            0xf1, 0xd9, 0xbf, 0xf0, 0x5e, 0x99, 0xa6, 0xf1, 0x00, 0xa4, 0x7f, 0xa0, 0xb1, 0x6b,
            0xd8, 0xbe, 0xef, 0xa0, 0xa1, 0xa5, 0x33, 0x9c, 0xc3, 0x95, 0xaa, 0x9f,
        ];

        let mut rng = FuzzRng::new(&buf[..]);
        let mut dest = Vec::with_capacity(buf.len());
        for chunk in buf.chunks(11) {
            dest.resize(chunk.len(), 0);
            rng.fill_bytes(&mut dest);

            assert_eq!(chunk, &*dest);
        }

        dest.resize(97, 0x2c);
        rng.fill_bytes(&mut dest);

        let mut zero_buf = Vec::with_capacity(dest.len());
        zero_buf.resize(dest.len(), 0);

        assert_eq!(zero_buf, dest);
    }

    #[test]
    fn try_fill_bytes() {
        let buf = &[
            0xdb, 0x35, 0xad, 0x4e, 0x9d, 0xf5, 0x2d, 0xf6, 0x0d, 0xc5, 0xd2, 0xfc, 0x9f, 0x4c,
            0xb5, 0x12, 0xe3, 0x78, 0x40, 0x8d, 0x8b, 0xa1, 0x5c, 0xfe, 0x66, 0x49, 0xa9, 0xc0,
            0x43, 0xa0, 0x95, 0xae, 0x31, 0x99, 0xd2, 0xaa, 0xbc, 0x85, 0x9e, 0x4b, 0x08, 0xca,
            0x59, 0x21, 0x2b, 0x66, 0x37, 0x6a, 0xb9, 0xb2, 0xd8, 0x71, 0x84, 0xdd, 0xf6, 0x47,
            0xa5, 0xb9, 0x87, 0x9f, 0x24, 0x97, 0x01, 0x65, 0x15, 0x38, 0x01, 0xd6, 0xb6, 0xf2,
            0x80,
        ];
        let mut rng = FuzzRng::new(&buf[..]);
        let mut dest = Vec::with_capacity(buf.len());
        for chunk in buf.chunks(13) {
            dest.resize(chunk.len(), 0);
            rng.try_fill_bytes(&mut dest)
                .expect("failed to fill bytes while data is remaining");

            assert_eq!(chunk, &*dest);
        }

        dest.resize(buf.len(), 0);
        rng.try_fill_bytes(&mut dest)
            .expect_err("successfully filled bytes when no data is remaining");
    }

    #[test]
    fn try_fill_bytes_partial() {
        let buf = &[
            0x8b, 0xe3, 0x20, 0x8d, 0xe0, 0x0b, 0xbe, 0x51, 0xa6, 0xec, 0x8a, 0xb5, 0xd6, 0x17,
            0x04, 0x3f, 0x87, 0xae, 0xc8, 0xe8, 0xf8, 0xe7, 0xd4, 0xbd, 0xf3, 0x4e, 0x74, 0xcf,
            0xbf, 0x0e, 0x9d, 0xe5, 0x78, 0xc3, 0xe6, 0x44, 0xb8, 0xd1, 0x40, 0xda, 0x63, 0x9f,
            0x48, 0xf4, 0x09, 0x9c, 0x5c, 0x5f, 0x36, 0x0b, 0x0d, 0x2b, 0xe3, 0xc7, 0xcc, 0x3e,
            0x9a, 0xb9, 0x0a, 0xca, 0x6d, 0x90, 0x77, 0x3b, 0x7a, 0x50, 0x16, 0x13, 0x5d, 0x20,
            0x70, 0xc0, 0x88, 0x04, 0x9c, 0xac, 0x2b, 0xd6, 0x61, 0xa0, 0xbe, 0xa4, 0xff, 0xbd,
            0xac, 0x9c, 0xa1, 0xb2, 0x95, 0x26, 0xeb, 0x99, 0x46, 0x67, 0xe4, 0xcd, 0x88, 0x7b,
            0x20, 0x4d, 0xb2, 0x92, 0x40, 0x9f, 0x1c, 0xbd, 0xba, 0x22, 0xff, 0xca, 0x89, 0x3c,
            0x3b,
        ];

        let mut rng = FuzzRng::new(&buf[..]);
        let mut dest = Vec::with_capacity(buf.len());
        dest.resize((buf.len() / 2) + 1, 0);

        // The first time should be successful because there is enough data left
        // in the buffer.
        rng.try_fill_bytes(&mut dest).expect("failed to fill bytes");
        assert_eq!(&buf[..dest.len()], &*dest);

        // The second time should fail because while there is data in the buffer it
        // is not enough to fill `dest`.
        rng.try_fill_bytes(&mut dest)
            .expect_err("filled bytes with insufficient data in buffer");

        // This should succeed because `dest` is exactly big enough to hold all the remaining
        // data in the buffer.
        dest.resize(buf.len() - dest.len(), 0);
        rng.try_fill_bytes(&mut dest)
            .expect("failed to fill bytes with exact-sized buffer");
        assert_eq!(&buf[buf.len() - dest.len()..], &*dest);
    }
}
