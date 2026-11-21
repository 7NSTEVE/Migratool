using System.Text.Json;
namespace Demo;
public static class Invoice {
    public static long Total(long cents, int rate) {
        if (cents < 0 || rate < 0 || rate > 100) throw new ArgumentOutOfRangeException(nameof(cents));
        return checked(cents + (cents * rate + 50) / 100);
    }
    public static int Main() {
        try {
            using var input = JsonDocument.Parse(Console.In.ReadToEnd());
            long cents = input.RootElement.GetProperty("cents").GetInt64();
            int rate = input.RootElement.GetProperty("rate").GetInt32();
            if (cents < 0 || rate < 0 || rate > 100) {
                Console.WriteLine("{\"error\":\"out_of_range\"}"); return 2;
            }
            long total = Total(cents, rate);
            File.WriteAllText("receipt.txt", "total=" + total.ToString(System.Globalization.CultureInfo.InvariantCulture), new System.Text.UTF8Encoding(false));
            Console.WriteLine(JsonSerializer.Serialize(new { total_cents = total, currency = "CRC" }));
            return 0;
        } catch (Exception) { Console.WriteLine("{\"error\":\"invalid_input\"}"); return 2; }
    }
}
