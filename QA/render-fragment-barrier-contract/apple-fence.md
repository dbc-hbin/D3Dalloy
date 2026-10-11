<!--
{
  "availability" : [
    "iOS: 10.0.0 -",
    "iPadOS: 10.0.0 -",
    "macCatalyst: 13.1.0 -",
    "macOS: 10.13.0 -",
    "tvOS: 10.0.0 -",
    "visionOS: 1.0.0 -"
  ],
  "documentType" : "symbol",
  "framework" : "Metal",
  "identifier" : "/documentation/Metal/MTLRenderCommandEncoder/waitForFence(_:before:)",
  "metadataVersion" : "0.1.0",
  "role" : "Instance Method",
  "symbol" : {
    "kind" : "Instance Method",
    "modules" : [
      "Metal"
    ],
    "preciseIdentifier" : "c:objc(pl)MTLRenderCommandEncoder(im)waitForFence:beforeStages:"
  },
  "title" : "waitForFence(_:before:)"
}
-->

# waitForFence(_:before:)

Encodes a command that instructs the GPU to pause before starting one or more stages of the render pass until a pass updates a fence.

```
func waitForFence(_ fence: any MTLFence, before stages: MTLRenderStages)
```

## Parameters

`fence`

A fence that the pass waits for before running the stages you pass to `stages`.

`stages`

The render stages that need to wait for another pass to update `fence` before they run.

## Discussion

Synchronize memory operations of a render pass that access resources with an [`MTLFence`](/documentation/Metal/MTLFence).
This method instructs the GPU to wait until another pass updates `fence`
before running the stages you pass to the `stages` parameter.
The fence indicates when the pass can access those resources without a race condition.

For more information about synchronization with fences, see:

- [Resource synchronization](/documentation/Metal/resource-synchronization)
- [Synchronizing passes with a fence](/documentation/Metal/synchronizing-passes-with-a-fence)

### Reuse a fence by waiting first and updating second

When encoding a render pass that reuses a fence, wait for other passes
to update the fence before repurposing that fence to notify subsequent passes with an update:

1. Call the [`waitForFence(_:before:)`](/documentation/Metal/MTLRenderCommandEncoder/waitForFence(_:before:)) method before encoding commands that need to wait for other passes.
2. Call the [`updateFence(_:after:)`](/documentation/Metal/MTLRenderCommandEncoder/updateFence(_:after:)) method after encoding commands that later passes depend on.

The GPU driver evaluates the fences that apply to the pass and the commands that depend on those fences when your app commits the enclosing [`MTLCommandBuffer`](/documentation/Metal/MTLCommandBuffer).

> Warning:
> Don’t update a fence and then wait for the same fence within a pass because it can create a GPU deadlock.

To synchronize different stages within a single pass, create an *intrapass barrier*
because a fence can only synchronize memory operations between different passes.
For more information, see [Synchronizing stages within a pass](/documentation/Metal/synchronizing-stages-within-a-pass).

---

Copyright &copy; 2026 Apple Inc. All rights reserved. | [Terms of Use](https://www.apple.com/legal/internet-services/terms/site.html) | [Privacy Policy](https://www.apple.com/privacy/privacy-policy)

Wall time: 0.22 seconds