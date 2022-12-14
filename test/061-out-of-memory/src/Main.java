/*
 * Copyright (C) 2008 The Android Open Source Project
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

import java.util.Arrays;
import java.util.LinkedList;

/**
 * Exercise the construction and throwing of OutOfMemoryError.
 */
public class Main {
    public static void main(String args[]) {
        System.out.println("tests beginning");
        $noinline$testHugeArray();
        $noinline$testOomeLarge();
        $noinline$testOomeSmall();
        $noinline$testOomeToCharArray();
        System.out.println("tests succeeded");
    }

    private static int[] $noinline$testHugeArray() {
        int[] tooBig = null;
        try {
            final int COUNT = 32768*32768 + 4;
            tooBig = new int[COUNT];
        } catch (OutOfMemoryError oom) {
            System.out.println("Got expected huge-array OOM");
        }
        return tooBig;
    }

    private static void $noinline$testOomeLarge() {
        System.out.println("testOomeLarge beginning");

        Boolean sawEx = false;
        byte[] a;

        try {
            // Just shy of the typical max heap size so that it will actually
            // try to allocate it instead of short-circuiting.
            a = new byte[(int) Runtime.getRuntime().maxMemory() - 32];
        } catch (OutOfMemoryError oom) {
            sawEx = true;
        }

        if (!sawEx) {
            throw new RuntimeException("Test failed: " +
                    "OutOfMemoryError not thrown");
        }

        System.out.println("testOomeLarge succeeded");
    }

    /* Do this in another method so that the GC has a chance of freeing the
     * list afterwards.  Even if we null out list when we're done, the conservative
     * GC may see a stale pointer to it in a register.
     */
    private static boolean testOomeSmallInternal() {
        final int LINK_SIZE = 6 * 4; // estimated size of a LinkedList's node

        LinkedList<Object> list = new LinkedList<Object>();

        /* Allocate progressively smaller objects to fill up the entire heap.
         */
        int objSize = 1 * 1024 * 1024;
        while (objSize >= LINK_SIZE) {
            boolean sawEx = false;
            try {
                for (int i = 0; i < Runtime.getRuntime().maxMemory() / objSize; i++) {
                    list.add((Object)new byte[objSize]);
                }
            } catch (OutOfMemoryError oom) {
                sawEx = true;
            }

            if (!sawEx) {
                return false;
            }

            objSize = (objSize * 4) / 5;
        }

        return true;
    }

    private static void $noinline$testOomeSmall() {
        System.out.println("testOomeSmall beginning");
        if (!testOomeSmallInternal()) {
            /* Can't reliably throw this from inside the internal function, because
             * we may not be able to allocate the RuntimeException.
             */
            throw new RuntimeException("Test failed: " +
                    "OutOfMemoryError not thrown while filling heap");
        }
        System.out.println("testOomeSmall succeeded");
    }

    private static Object $noinline$testOomeToCharArray() {
        Object[] o = new Object[2000000];
        String test = "test";
        int i = 0;
        try {
            for (; i < o.length; ++i) o[i] = new char[1000000];
        } catch (OutOfMemoryError oom) {}
        try {
            for (; i < o.length; ++i) {
                o[i] = test.toCharArray();
            }
        } catch (OutOfMemoryError oom) {
            o = null;
            System.out.println("Got expected toCharArray OOM");
        }
        return o;
    }
}
