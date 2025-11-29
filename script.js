// ---------------- Supabase Setup ----------------
const SUPABASE_URL = "https://akxcjabakrvfaevdfwru.supabase.co";
const SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImFreGNqYWJha3J2ZmFldmRmd3J1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDkxMjMwMjUsImV4cCI6MjA2NDY5OTAyNX0.kykki4uVVgkSVU4lH-wcuGRdyu2xJ1CQkYFhQq_u08w";

const supabase = window.supabase.createClient(SUPABASE_URL, SUPABASE_ANON_KEY);

// List of devices
const devices = ["uno_1", "uno_2", "uno_3"];

// Main function to load all data
async function loadData() {
    const container = document.getElementById("container");
    container.innerHTML = ""; // clear old cards

    let totalBreakfast = 0;
    let totalLunch = 0;
    let totalDinner = 0;
    let grandTotal = 0;

    for (let sensor of devices) {

        // Fetch each device data
        const { data, error } = await supabase
            .from("unodari_token")
            .select("*")
            .eq("sensor_id", sensor)
            .single();

        if (error || !data) continue;

        const { breakfast, lunch, dinner, total, timestamp } = data;

        // accumulate totals (y-axis)
        totalBreakfast += breakfast;
        totalLunch += lunch;
        totalDinner += dinner;
        grandTotal += total;

        // Make card
        const card = document.createElement("div");
        card.className = "card";

        card.innerHTML = `
            <h3>${sensor.toUpperCase()}</h3>

            <div class="row"><span>Breakfast:</span> <b>${breakfast}</b></div>
            <div class="row"><span>Lunch:</span> <b>${lunch}</b></div>
            <div class="row"><span>Dinner:</span> <b>${dinner}</b></div>

            <hr>
            <div class="row"><span>Total:</span> <b>${total}</b></div>

            <p class="timestamp">Last Updated: ${formatTimestamp(timestamp)}</p>
        `;

        container.appendChild(card);
    }

    // Update totals section (y-axis totals)
    document.getElementById("total-breakfast").innerText = totalBreakfast;
    document.getElementById("total-lunch").innerText = totalLunch;
    document.getElementById("total-dinner").innerText = totalDinner;
    document.getElementById("grand-total").innerText = grandTotal;
}

// Format timestamp nicely
function formatTimestamp(ts) {
    const date = new Date(ts);
    return date.toLocaleString();
}

// First load
loadData();

// Auto-refresh every 10 seconds
setInterval(loadData, 10000);
