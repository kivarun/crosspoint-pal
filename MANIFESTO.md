# Fork Manifesto

This document defines the product direction of this fork. Upstream CrossPoint
remains the preferred source for reader-core improvements; upstream's
`SCOPE.md` describes upstream acceptance policy, not the product boundary of
this fork.

## Purpose

The device should give its owner more reasons to keep it nearby and take it
along.

Reading remains a first-class use case, but it is not the only one. The goal is
to use the strengths of a small e-ink device — persistent display, very low
power while static, sunlight readability, calm presentation, and physical
separation from a phone or laptop — for useful companion functions that other
devices do not provide as well.

The intended product is a **quiet e-ink companion**: a reader when you want to
read, an ambient display when it sits on a desk, and a small persistent
reference screen when you carry it.

It is not a miniature general-purpose computer.

## Use the properties of e-ink

A feature belongs here when it gains something important from being:

- always visible without keeping a bright screen awake;
- low-power when its contents change rarely;
- glanceable rather than attention-seeking;
- persistent across minutes or hours;
- physically separate from the phone or computer that produced the data;
- useful as a small dedicated surface that can be reached faster than opening
  an app and finding the same information again.

A useful question for every proposed feature is:

> Does this become meaningfully better because it lives on a small persistent
> e-ink screen?

If the answer is no, and a phone or laptop already does the job better, it
probably does not belong in the firmware.

## In scope

Examples of the intended product direction include:

- reading and improvements to the reading experience;
- custom sleep screens and on-device rendering controls;
- photo-frame and slideshow modes;
- notifications mirrored from a phone;
- an ambient desk display for useful, slowly changing information;
- a companion display for a phone or computer;
- cards containing QR codes, tickets, passes, identifiers, addresses, short
  instructions, shopping or packing lists, and similar reference material;
- short notes and small documents that are useful to keep immediately
  available;
- status or reference views that benefit from staying visible for a long time;
- low-power companion connectivity, with the phone or computer doing the heavy
  work where that produces a better system.

On-device image rendering controls are in scope when they tune how an existing
image appears on the actual e-ink panel. The panel is the target and the device
is the only place where the final quantized result can be judged directly.

## Out of scope by default

The project is not trying to reproduce a phone, PDA, or desktop environment.

Features such as these require a strong separate justification:

- games;
- calculators;
- general-purpose image editors;
- text editors and authoring suites;
- arbitrary desktop-style applications;
- high-refresh interactive interfaces;
- features whose main justification is only that the ESP32 is technically
  capable of running them.

The distinction is utility, not feature count. A QR card that remains visible
for hours has a natural reason to exist on e-ink. A calculator generally does
not.

## The phone and computer are partners, not competitors

The companion model should use each device for what it does well.

A phone or computer is the natural place for:

- heavy computation;
- authoring and complex editing;
- network-heavy operations;
- cameras and media capture;
- configuration that needs a rich interface.

The e-ink device is the natural place for:

- persistent display;
- calm notification and status presentation;
- frequently referenced small pieces of information;
- low-power ambient output;
- content that should remain visible without occupying the phone.

Bluetooth and Wi-Fi are means to connect those roles. They should not turn the
reader into a permanently awake network terminal. Prefer low-power control and
small transfers, and use higher-power connectivity only when the workflow
actually needs it.

## Upstream first where the goals align

A downstream fork should not become an excuse to stop contributing.

Changes that are generally useful to CrossPoint and fit upstream's product
boundary should be proposed upstream. Shared fixes, HAL and SDK improvements,
reader improvements, portability work, and reusable low-level primitives are
better maintained once.

This fork exists for product direction that upstream intentionally does not
want to own. Downstream-specific features should remain cleanly separated from
the upstream reader core where practical so that upstream changes can continue
to be integrated without unnecessary conflict.

## Engineering principles

### Evidence before assumptions

Code, logs, measurements, hardware tests, and reproducible experiments outrank
names, comments, plausible stories, or architectural expectations.

Distinguish confirmed facts from probable explanations and unknowns. Do not
turn a hypothesis into product state merely because it is convenient.

### Presumption of non-existence

For product and design decisions, anything not established by evidence does
not exist.

A capability, domain entity, lifecycle state, compatibility requirement,
failure mode, abstraction, or future requirement needs an accepted contract, a
reachable production path, an authoritative dependency guarantee, a
reproducible observation, or an explicit current use case.

A suggestive name, field, comment, TODO, stale branch, backend object, or
possible future is a lead to investigate, not proof.

Do not build frameworks, compatibility layers, background systems, or
configuration switches for hypothetical future needs.

### Broad analysis, narrow change

Investigate as widely as necessary to understand the real owner, boundaries,
callers, state, and failure modes of a problem.

Then change only what is necessary to solve the demonstrated problem and keep
its contracts coherent. Do not turn a focused fix into opportunistic cleanup.

### One concept, one canonical owner

Equivalent semantics should have one canonical term, one source of truth, and
one production owner.

Do not create a parallel lifecycle, state model, renderer, validation path, or
compatibility mechanism merely because extending the existing owner is
inconvenient. If a new mechanism replaces an old one, migrate callers and
remove the obsolete path unless a demonstrated compatibility requirement
requires both.

### Entropy is a cost

Every noun, state, abstraction, flag, special case, alias, adapter, and
production path increases the amount of system that must be understood and
maintained.

New structure must earn its existence. Prefer extending an existing concept,
deleting obsolete distinctions, and keeping the number of authoritative paths
small.

### Explicit contracts, fail closed

Authority, persistence, lifecycle transitions, and external interfaces should
be explicit.

Missing or incoherent state must not silently become permission, success, or a
guessed fallback. Preserve evidence, report the failure clearly, and recover
only through an accepted contract.

### Compatibility must be justified

Compatibility is a product requirement when a real user, stored state, public
contract, or supported dependency needs it. It is not a default reason to keep
every historical behavior forever.

### Reproducibility and provenance matter

It should be possible to identify which source revision, build inputs,
configuration, and device produced an observed result.

Tests and reports should make the evidence chain clear enough that another
developer can reproduce the conclusion.

### Hardware truth wins

Simulator, host, and compile tests are necessary but do not prove physical
e-ink behavior, input ergonomics, power behavior, or radio behavior.

When a claim depends on the real device, hardware UAT is a distinct and
necessary form of evidence.

### Principles are tools, not commandments

Design principles exist to improve the product, reduce complexity, and protect
users. They are not axioms to defend at the expense of a demonstrated workflow.

When a principle appears to conflict with an observed user need, identify the
goal the principle protects and choose the smallest bounded solution that
preserves that goal without pushing avoidable complexity or inconvenience onto
the user. Minimalism, scope, abstraction boundaries, and consistency are means,
not ends.

An exception is not permission for ad hoc design: it must be explicit,
evidence-based, and narrow. If repeated evidence shows that a principle
consistently produces worse outcomes, change the principle instead of forcing
the product to conform to it.
