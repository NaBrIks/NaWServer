using System.Text.Json;
using Showcase.Model;

namespace Showcase.Host;

// Independent one-way diagnostics. Four bounded subscriptions, one sample/second,
// no replay or queued history; a stalled write closes only its subscription.
public sealed class DiagnosticEvents {
    private readonly object gate = new();
    private int active;
    public async Task Handle(HttpContext context, DeviceModel model, Traffic traffic, SocketHub sockets) {
        lock (gate) {
            if (active >= 4) { context.Response.StatusCode = 503; return; }
            active++;
        }
        try {
            context.Response.ContentType = "text/event-stream; charset=utf-8";
            context.Response.Headers.CacheControl = "no-cache";
            using var timer = new PeriodicTimer(TimeSpan.FromSeconds(1));
            do {
                if (model.NetworkOffline) break;
                var sample = model.Summary(traffic.Snapshot(), sockets.Count);
                var payload = JsonSerializer.Serialize(new {
                    v = 2, source = "simulator", boot_id = sample.BootId, seq = sample.Seq,
                    captured_ms = sample.CapturedMs, cpu_busy_pct = sample.Data.CpuBusyPct,
                    free_bytes = sample.Data.Memory[0].FreeBytes, task_count = sample.Data.TaskCount
                });
                using var deadline = CancellationTokenSource.CreateLinkedTokenSource(context.RequestAborted);
                deadline.CancelAfter(TimeSpan.FromSeconds(5));
                await context.Response.WriteAsync($"event: diagnostic\nid: {sample.BootId}:{sample.Seq}\nretry: 1500\ndata: {payload}\n\n", deadline.Token);
                await context.Response.Body.FlushAsync(deadline.Token);
            } while (await timer.WaitForNextTickAsync(context.RequestAborted));
        } catch (OperationCanceledException) { context.Abort(); }
          catch (IOException) { context.Abort(); }
        finally { lock (gate) active--; }
    }
}
