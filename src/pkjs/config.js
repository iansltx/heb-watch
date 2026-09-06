// Clay config page for HEB List.
// Rendered locally by the Pebble app as a data: URI — no hosting needed.
module.exports = [
  {
    type: 'heading',
    defaultValue: 'HEB List'
  },
  {
    type: 'text',
    defaultValue: 'Paste the shared list URL from heb.com (Menu > Share list), ' +
      'e.g. https://www.heb.com/shopping-list/shared/12345678-1234-4321-8765-432109876543'
  },
  {
    type: 'section',
    items: [
      {
        type: 'input',
        messageKey: 'ListUrl',
        label: 'Shared list URL',
        defaultValue: ''
      },
      {
        type: 'toggle',
        messageKey: 'HideChecked',
        label: 'Hide checked items',
        defaultValue: false
      }
    ]
  },
  {
    type: 'submit',
    defaultValue: 'Save'
  }
];