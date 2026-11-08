# DockAwesome

<!-- impeccable:product-schema 1 -->

## Platform

ApolloOS on the Vampire V4 (68080 with AMMX and SAGA). There is no build for
other CPUs.

## Users

Workbench users with modern screen resolutions.

## Product Purpose

A small macOS-style dock with zoom, drag and drop, glass, and consistent icons.

## Stack

C, 68080 AMMX, and OS libraries.

## Operating Context

The tested setup is ApolloOS R9.5 on a Vampire 68080 with a 1280 × 720 RGB565
SAGA screen. Live glass needs a 16-bit SAGA screen; other screens use the
snapshot or opaque background.

## Capabilities and Constraints

Use `icon.library` to read Amiga icons. Keep OS control of the desktop and
reserve PiP2 for live glass. Use snapshot or opaque mode on other window stacks.

## Brand Commitments

Match the feel of the macOS Dock. Keep the user's native Amiga artwork.

## Open Decisions

Check which icon extensions other `icon.library` versions expose. Verify live
PiP on each new OS and driver before claiming support.
