<script setup>
const model = defineModel({
  type: [Boolean, Number, String],
  required: true,
});
const slots = defineSlots();
const props = defineProps({
  class: {
    type: String,
    default: ""
  },
  desc: {
    type: String,
    default: null
  },
  id: {
    type: String,
    required: true
  },
  label: {
    type: String,
    default: null
  },
  localePrefix: {
    type: String,
    default: "missing-prefix"
  },
  inverseValues: {
    type: Boolean,
    default: false,
  },
  default: {
    type: undefined,
    default: null,
  }
});

// Add the mandatory class values
const extendedClassStr = (() => {
  let values = props.class.split(" ");
  if (!values.includes("form-check")) {
    values.push("form-check");
  }
  return values.join(" ");
})();

// Map the value to boolean representation if possible, otherwise return null.
const mapToBoolRepresentation = (value) => {
  // Try literal values first
  if (value === true || value === false) {
    return { possibleValues: [true, false], value: value };
  }
  if (value === 1 || value === 0) {
    return { possibleValues: [1, 0], value: value };
  }

  const stringPairs = [
    ["true", "false"],
    ["1", "0"],
    ["enabled", "disabled"],
    ["enable", "disable"],
    ["yes", "no"],
    ["on", "off"]
  ];

  value = `${value}`.toLowerCase().trim();
  for (const pair of stringPairs) {
    if (value === pair[0] || value === pair[1]) {
      return { possibleValues: pair, value: value };
    }
  }

  return null;
}

// Determine the true/false values for the checkbox
const checkboxValues = (() => {
  const mappedValues = (() => {
    const boolValues = mapToBoolRepresentation(model.value);
    if (boolValues !== null) {
      return boolValues.possibleValues;
    }

    // Return fallback if nothing matches
    console.error(`Checkbox value ${model.value} did not match any acceptable pattern!`);
    return ["true", "false"];
  })();

  const truthyIndex = props.inverseValues ? 1 : 0;
  const falsyIndex = props.inverseValues ? 0 : 1;
  return { truthy: mappedValues[truthyIndex], falsy: mappedValues[falsyIndex] };
})();
const parsedDefaultPropValue = (() => {
  const boolValues = mapToBoolRepresentation(props.default);
  if (boolValues !== null) {
    // Convert truthy to true/false.
    return boolValues.value === boolValues.possibleValues[0];
  }

  return null;
})();

const labelField = props.label ?? `${props.localePrefix}.${props.id}`;
const descField = props.desc ?? `${props.localePrefix}.${props.id}_desc`;
const showDesc = props.desc !== "" || Object.entries(slots).length > 0;
const showDefValue = parsedDefaultPropValue !== null;
const defValue = parsedDefaultPropValue ? "_common.enabled_def_cbox" : "_common.disabled_def_cbox";
</script>

<template>
  <div :class="extendedClassStr">
    <label :for="props.id" :class="`form-check-label${showDesc ? ' mb-2' : ''}`">
      {{ $t(labelField) }}
      <div class="mt-0 form-text" v-if="showDefValue">
        {{ $t(defValue) }}
      </div>
    </label>
    <input type="checkbox"
           class="form-check-input"
           :id="props.id"
           v-model="model"
           :true-value="checkboxValues.truthy"
           :false-value="checkboxValues.falsy" />
    <div class="form-text" v-if="showDesc">
      {{ $t(descField) }}
      <slot></slot>
    </div>
  </div>
</template>
