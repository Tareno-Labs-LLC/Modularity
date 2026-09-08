using System;

namespace ModuCPP;

[HeadText("Sample Inspector")]
public class SampleInspectorManaged {
        private bool autoRotate = false;
        private bool ensureRigidbody = false;
        [DragSpeed(1.0f)]
        private Vec3 spinSpeed = new Vec3(0f, 45f, 0f);
        [DragSpeed(0.1f)]
        private Vec3 offset = new Vec3(0f, 1f, 0f);
        private ModuObject targetObject;

        private void ApplyAutoRotate(Context context, float deltaTime) {
            if (!autoRotate) return;
            context.Rotation = context.Rotation + (spinSpeed * deltaTime);
        }

        public void Begin(IntPtr ctx, float deltaTime) {
            var context = new Context(ctx);
            context.AutoSettingsFrom(this, save: false);
            if (ensureRigidbody) {
                context.EnsureRigidbody(useGravity: true, kinematic: false);
            }
            context.AddConsoleMessage("Managed script begin (C#)", ConsoleMessageType.Info);
        }

        public void Spec(IntPtr ctx, float deltaTime) {
            ApplyAutoRotate(new Context(ctx), deltaTime);
        }

        public void TestEditor(IntPtr ctx, float deltaTime) {
            ApplyAutoRotate(new Context(ctx), deltaTime);
        }

        public void TickUpdate(IntPtr ctx, float deltaTime) {
            ApplyAutoRotate(new Context(ctx), deltaTime);
        }
}
