// ---------------- Supabase Setup ----------------
const SUPABASE_URL = "https://akxcjabakrvfaevdfwru.supabase.co";
const SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImFreGNqYWJha3J2ZmFldmRmd3J1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDkxMjMwMjUsImV4cCI6MjA2NDY5OTAyNX0.kykki4uVVgkSVU4lH-wcuGRdyu2xJ1CQkYFhQq_u08w";

const supabase = window.supabase.createClient(SUPABASE_URL, SUPABASE_ANON_KEY);

// List of devices
const devices = ["uno_1", "uno_2", "uno_3"];

// Meal time ranges (in 24-hour format)
const mealTimes = {
    breakfast: { start: 6, end: 9 },    // 6 AM - 9 AM
    lunch: { start: 11, end: 15 },       // 11 AM - 3 PM
    dinner: { start: 17, end: 22 }       // 5 PM - 10 PM
};

// Function to determine current meal time
function getCurrentMealPeriod() {
    const hour = new Date().getHours();
    
    if (hour >= mealTimes.breakfast.start && hour < mealTimes.breakfast.end) {
        return 'breakfast';
    } else if (hour >= mealTimes.lunch.start && hour < mealTimes.lunch.end) {
        return 'lunch';
    } else if (hour >= mealTimes.dinner.start && hour < mealTimes.dinner.end) {
        return 'dinner';
    }
    return null;
}

// Function to highlight active meal rows
function highlightActiveMeal() {
    const activeMeal = getCurrentMealPeriod();
    
    // Remove all active classes
    document.querySelectorAll('.row.active').forEach(row => row.classList.remove('active'));
    
    if (activeMeal) {
        // Highlight all rows of the active meal type
        document.querySelectorAll(`.row .${activeMeal}`).forEach(element => {
            element.closest('.row').classList.add('active');
        });
    }
}

// Main function to load all data
async function loadData() {
    let totalBreakfast = 0;
    let totalLunch = 0;
    let totalDinner = 0;
    let grandTotal = 0;

    for (let sensor of devices) {

        // Get today's date in YYYY-MM-DD format
        const today = new Date().toISOString().split('T')[0];

        // Fetch each device data
        const { data, error } = await supabase
            .from("unodari_token")
            .select("*")
            .eq("sensor_id", sensor)
            .eq("date", today)
            .single();

        if (error || !data) continue;

        const { breakfast, lunch, dinner, total, timestamp } = data;

        // accumulate totals
        totalBreakfast += breakfast;
        totalLunch += lunch;
        totalDinner += dinner;
        grandTotal += total;

        // Update only the values in the static cards
        document.querySelector(`b.breakfast[data-device="${sensor}"]`).innerText = breakfast;
        document.querySelector(`b.lunch[data-device="${sensor}"]`).innerText = lunch;
        document.querySelector(`b.dinner[data-device="${sensor}"]`).innerText = dinner;
        document.querySelector(`b.total[data-device="${sensor}"]`).innerText = total;
        document.querySelector(`p.timestamp[data-device="${sensor}"]`).innerText = `Last Updated: ${formatTimestamp(timestamp)}`;
    }

    // Update totals section
    document.getElementById("total-breakfast").innerText = totalBreakfast;
    document.getElementById("total-lunch").innerText = totalLunch;
    document.getElementById("total-dinner").innerText = totalDinner;
    document.getElementById("grand-total").innerText = grandTotal;
    
    // Highlight active meal period
    highlightActiveMeal();
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

// Update highlight every minute to catch time changes
setInterval(highlightActiveMeal, 60000);
