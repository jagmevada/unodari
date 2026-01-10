// ---------------- Supabase Setup ----------------
const SUPABASE_URL = "https://akxcjabakrvfaevdfwru.supabase.co";
const SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImFreGNqYWJha3J2ZmFldmRmd3J1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDkxMjMwMjUsImV4cCI6MjA2NDY5OTAyNX0.kykki4uVVgkSVU4lH-wcuGRdyu2xJ1CQkYFhQq_u08w";

// Create a Supabase client instance. Use a different local name to avoid
// colliding with any existing global `supabase` identifier provided by
// other scripts or the environment.
const supabaseClient = window.supabase.createClient(SUPABASE_URL, SUPABASE_ANON_KEY);

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
    const hour = new Date().getHours(); // Add +<>
    
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
    const mealData = {
        breakfast: { devices: {}, total: 0, timestamp: '' },
        lunch: { devices: {}, total: 0, timestamp: '' },
        dinner: { devices: {}, total: 0, timestamp: '' }
    };
    reorderMealCards();
    let grandTotal = 0;

    // Get today's date with timezone consideration
    const now = new Date();
    const year = now.getFullYear();
    const month = String(now.getMonth() + 1).padStart(2, '0');
    const day = String(now.getDate()).padStart(2, '0');
    const today = `${year}-${month}-${day}`;

    // Fetch data for each device
    for (let sensor of devices) {
        

        const { data, error } = await supabaseClient
            .from("unodari_token")
            .select("*")
            .eq("sensor_id", sensor)
            .eq("date", today)
            .single();

        if (error || !data) continue;

        const { 
            breakfast_total, lunch_total, dinner_total, 
            breakfast_manual, lunch_manual, dinner_manual, 
            timestamp 
        } = data;

        // Populate manual inputs
        const meals = [
            { name: 'breakfast', value: breakfast_manual },
            { name: 'lunch', value: lunch_manual },
            { name: 'dinner', value: dinner_manual }
        ];

        meals.forEach(m => {
            const input = document.querySelector(`.inline-input[data-device="${sensor}"][data-meal="${m.name}"]`);
            if (input) {
                // Only set if not currently focused to avoid overwriting user typing (basic safeguard)
                // However, usually these auto-refresh, so might be annoying. 
                // Given the requirement is "filled in", we set it.
                if (document.activeElement !== input) {
                    input.value = m.value || ''; 
                }
            }
        });

        // Organize by meal type (using Total columns which include Manual + Auto)
        mealData.breakfast.devices[sensor] = breakfast_total || 0;
        mealData.breakfast.total += (breakfast_total || 0);
        mealData.breakfast.timestamp = timestamp;

        mealData.lunch.devices[sensor] = lunch_total || 0;
        mealData.lunch.total += (lunch_total || 0);
        mealData.lunch.timestamp = timestamp;

        mealData.dinner.devices[sensor] = dinner_total || 0;
        mealData.dinner.total += (dinner_total || 0);
        mealData.dinner.timestamp = timestamp;

        // Calculate grand total from the fetched totals
        grandTotal += (breakfast_total || 0) + (lunch_total || 0) + (dinner_total || 0);
    }

    // Update meal cards with device counts
    for (const meal of ['breakfast', 'lunch', 'dinner']) {
        const mealInfo = mealData[meal];
        
        // Update individual device counts
        for (const device of devices) {
            const count = mealInfo.devices[device] || 0;
            const element = document.querySelector(`b.device-count[data-device="${device}"][data-meal="${meal}"]`);
            if (element) element.innerText = count;
        }

        // Update meal total
        const totalElement = document.querySelector(`b.meal-total[data-meal="${meal}"]`);
        if (totalElement) totalElement.innerText = mealInfo.total;

        // Update timestamp
        const timestampElement = document.querySelector(`p.timestamp[data-meal="${meal}"]`);
        if (timestampElement) {
            timestampElement.innerText = `Last Updated: ${mealInfo.timestamp ? formatTimestamp(mealInfo.timestamp) : '-'}`;
        }
    }

    // Update totals section
    document.getElementById("total-breakfast").innerText = mealData.breakfast.total;
    document.getElementById("total-lunch").innerText = mealData.lunch.total;
    document.getElementById("total-dinner").innerText = mealData.dinner.total;
    document.getElementById("grand-total").innerText = grandTotal;
    
    // Reorder cards with active meal on top (after data is loaded)
    reorderMealCards();
    
    // Highlight active meal period
    highlightActiveMeal();
}

// Function to reorder meal cards with active meal on top
function reorderMealCards() {
    const container = document.getElementById('container');
    const activeMeal = getCurrentMealPeriod();
    
    const meals = ['breakfast', 'lunch', 'dinner'];
    
    // Mark inactive cards with inactive class
    meals.forEach(meal => {
        const card = document.querySelector(`[data-meal="${meal}"]`);
        if (card) {
            if (activeMeal && meal !== activeMeal) {
                card.classList.add('inactive');
            } else {
                card.classList.remove('inactive');
            }
        }
    });
    
    if (!activeMeal) return;

    const mealOrder = [activeMeal, ...meals.filter(m => m !== activeMeal)];

    mealOrder.forEach(meal => {
        const card = document.querySelector(`[data-meal="${meal}"]`);
        if (card) {
            container.appendChild(card); // Move to end (which reorders them)
        }
    });
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


// Function to send individual manual data to Supabase
async function sendIndividualData(meal, device) {
    const input = document.querySelector(`.inline-input[data-meal="${meal}"][data-device="${device}"]`);
    const btn = document.querySelector(`button[onclick="sendIndividualData('${meal}', '${device}')"]`);
    
    if (!input || !input.value) {
        alert("Please enter a value.");
        return;
    }

    const value = parseInt(input.value, 10);
    if (isNaN(value)) {
        alert("Please enter a valid number.");
        return;
    }

    const now = new Date();
    const year = now.getFullYear();
    const month = String(now.getMonth() + 1).padStart(2, '0');
    const day = String(now.getDate()).padStart(2, '0');
    const today = `${year}-${month}-${day}`;

    // Disable button to prevent double clicks
    if (btn) {
        btn.disabled = true;
        btn.innerText = "...";
    }

    const updateObj = {};
    updateObj[`${meal}_manual`] = value;

    try {
        const { error } = await supabaseClient
            .from("unodari_token")
            .update(updateObj)
            .eq("sensor_id", device)
            .eq("date", today);

        if (error) throw error;

        // alert("Added successfully!");
        input.value = ''; // Clear input

        // Refresh data
        loadData();

    } catch (err) {
        console.error("Error updating manual data:", err);
        alert("Failed to update data.");
    } finally {
         if (btn) {
            btn.disabled = false;
            btn.innerText = "Add";
        }
    }
}

